import type { SessionRateLimit } from 'claude-code'

import { baseName, describeCall } from './describe'
import { apiCall, quietly, type HaConfig, type Json } from './ha'
import type { Host } from './host'
import { type DisplayAnswer, type ReportedQuestion, toReported } from './questions'

/** How a session is doing, as the integration and the display show it. */
export type WorkState = 'idle' | 'working' | 'asking' | 'permission'

export type ContextUsage = { percent?: number; window: number; tokens?: number }

/** One open question's fate, as the integration reports it back with every report. */
export type QuestionStatus = {
  status: 'open' | 'answered' | 'cancelled' | 'unknown'
  /** Who answered: `display` or `pc`. */
  by?: string
  /** The display's answer: option ids per part id. */
  answer?: DisplayAnswer
  nonce?: string
}

export type ReportReply = {
  ok: boolean
  handle?: string
  questions: Record<string, QuestionStatus>
}

export type ClaimReply = {
  accepted: boolean
  status?: string
  by?: string
  answer?: DisplayAnswer
}

/** Every 15 s a session says it is still there; every 1.5 s while one of its questions is open. */
export const HEARTBEAT_MS = 15_000
export const POLL_MS = 1_500

type PendingAsk = { tool: string; detail: string; hinted: boolean }

type Waiter = (status: QuestionStatus) => void

/**
 * The bridge of one Claude session: what it reports to Home Assistant and what it hears back. One per session id;
 * the module keeps the current one (`Bridge`). Nothing here is shared with another session: a reload of the mod, a
 * /clear or a resume starts a new one under its own session id.
 */
export class SessionBridge {
  seq = 0
  name = ''
  model = ''
  cwd = ''
  working = false
  context: ContextUsage | null = null
  limits: SessionRateLimit[] = []
  /** When `limits` was measured, in the host clock's milliseconds; 0 for never. */
  limitsAt = 0
  /** The questions of this session the display may answer, by tool_use_id. */
  readonly questions = new Map<string, ReportedQuestion>()
  /** Tool calls whose permission ask is still open, by tool_use_id. */
  readonly pendingAsks = new Map<string, PendingAsk>()
  private readonly waiters = new Map<string, Waiter[]>()
  private lastReportAt = 0
  private dirty = true
  private ended = false

  constructor(
    readonly instance: string,
    readonly sessionId: string,
    readonly cfg: HaConfig,
  ) {}

  /** The state the integration shows for this session, from what the hooks saw. */
  state(): WorkState {
    if (this.questions.size > 0) return 'asking'
    for (const ask of this.pendingAsks.values()) if (ask.hinted) return 'permission'
    return this.working ? 'working' : 'idle'
  }

  /** The permission hint's text while one is up, else "". */
  detail(): string {
    for (const ask of this.pendingAsks.values()) if (ask.hinted) return ask.detail
    return ''
  }

  /** Marks the state as changed: the next tick reports at once instead of waiting for the heartbeat. */
  touch() {
    this.dirty = true
  }

  /** The report the integration gets: the whole state of the session, so a lost report costs nothing. */
  body(now = 0): Json {
    this.seq += 1
    return {
      instance: this.instance,
      session: this.sessionId,
      seq: this.seq,
      name: this.name,
      instance_name: this.cfg.instanceName,
      model: this.model,
      cwd: this.cwd,
      state: this.state(),
      detail: this.detail(),
      context: this.context
        ? { percent: this.context.percent ?? null, tokens: this.context.tokens ?? null, window: this.context.window }
        : null,
      limits: this.limits.map(l => ({ kind: l.kind, percent: l.percentUsed, resets_at: l.resetsAt ?? null })),
      // Seconds since this session measured its limits: an idle session repeats an old reading on every
      // heartbeat, and the integration keeps the freshest reading of the account, not the last one that arrived.
      limits_age: this.limitsAt && now >= this.limitsAt ? Math.round((now - this.limitsAt) / 1000) : null,
      questions: Array.from(this.questions.values()),
    }
  }

  /** Sends the report; hands every open question's status to whoever waits for it. Undefined when HA is down. */
  async report(host: Host): Promise<ReportReply | undefined> {
    if (this.ended) return undefined
    const now = await host.now()
    const reply = await quietly(host, () => apiCall(host, this.cfg, '/report', this.body(now)))
    if (reply === undefined) return undefined
    this.lastReportAt = now
    this.dirty = false
    const statuses = (reply.questions ?? {}) as Record<string, QuestionStatus>
    for (const [id, status] of Object.entries(statuses)) {
      const question = this.questions.get(id)
      if (!question) continue
      if (status.nonce !== undefined && status.nonce !== question.nonce) continue
      if (status.status === 'answered' || status.status === 'cancelled') this.settleQuestion(id, status)
    }
    return { ok: reply.ok === true, handle: typeof reply.handle === 'string' ? reply.handle : undefined, questions: statuses }
  }

  /** The timer's tick: a report when something changed, a question is open, or the heartbeat is due. */
  async tick(host: Host) {
    const now = await host.now()
    const due = now - this.lastReportAt >= HEARTBEAT_MS
    if (this.dirty || this.questions.size > 0 || due) await this.report(host)
  }

  /** Registers a question for the display and reports it at once. */
  async ask(host: Host, id: string, raw: unknown[]): Promise<ReportedQuestion | null> {
    const question = toReported(id, raw)
    if (!question) return null
    this.questions.set(id, question)
    this.touch()
    await this.report(host)
    return question
  }

  /** Resolves once the integration says the question was answered (by the display) or cancelled. */
  waitFor(id: string): Promise<QuestionStatus> {
    return new Promise(resolve => {
      const list = this.waiters.get(id) ?? []
      list.push(resolve)
      this.waiters.set(id, list)
    })
  }

  /**
   * The PC answered (or dismissed) the question: asks the integration to settle it that way. Accepted, the PC's
   * answer stands; refused, the display answered first and its answer comes back. Undefined when HA is down.
   */
  async claim(host: Host, id: string, outcome: 'answered' | 'cancelled'): Promise<ClaimReply | undefined> {
    const question = this.questions.get(id)
    if (!question) return undefined
    const reply = await quietly(host, () =>
      apiCall(host, this.cfg, '/claim', {
        instance: this.instance,
        session: this.sessionId,
        id,
        nonce: question.nonce,
        by: 'pc',
        outcome,
      }),
    )
    if (reply === undefined) return undefined
    return {
      accepted: reply.accepted === true,
      status: typeof reply.status === 'string' ? reply.status : undefined,
      by: typeof reply.by === 'string' ? reply.by : undefined,
      answer: reply.answer !== null && typeof reply.answer === 'object' ? (reply.answer as DisplayAnswer) : undefined,
    }
  }

  /** Takes a question out of the report (it was answered or cancelled) and wakes whoever waits for it. */
  settleQuestion(id: string, status: QuestionStatus) {
    this.questions.delete(id)
    this.touch()
    const list = this.waiters.get(id)
    this.waiters.delete(id)
    for (const resolve of list ?? []) resolve(status)
  }

  /** The turn ended, or was aborted: every question and ask of this session is over. Only this session's. */
  settleAll() {
    for (const id of Array.from(this.questions.keys())) this.settleQuestion(id, { status: 'cancelled' })
    this.pendingAsks.clear()
    this.working = false
    this.touch()
  }

  /** A permission ask opened; it becomes a hint when it is still open after the delay. */
  askPermission(toolUseId: string, tool: string, input: unknown) {
    this.pendingAsks.set(toolUseId, { tool, detail: describeCall(tool, input), hinted: false })
  }

  /** True when the ask was still open and is a hint from now on. */
  hintIfStillOpen(toolUseId: string): boolean {
    const ask = this.pendingAsks.get(toolUseId)
    if (!ask || ask.hinted) return false
    ask.hinted = true
    this.touch()
    return true
  }

  /** The tool ran or was refused: the person answered on the PC. */
  settleAsk(toolUseId: string) {
    if (this.pendingAsks.delete(toolUseId)) this.touch()
  }

  /** Tells the integration the session is over; nothing is reported after that. */
  async end(host: Host, reason: string) {
    if (this.ended) return
    this.ended = true
    for (const id of Array.from(this.questions.keys())) this.settleQuestion(id, { status: 'cancelled' })
    await quietly(host, () => apiCall(host, this.cfg, '/end', { instance: this.instance, session: this.sessionId, reason }))
  }
}

const INSTANCE_KEY = 'instanceId'

/**
 * The module's view: the bridge of the session this engine runs. The engine runs one session at a time (a /clear or a
 * resume goes on under another id), so every hook asks for the current one and a new id starts a new bridge.
 */
export class Bridge {
  private current: SessionBridge | null = null
  private instance: string | null = null
  private timer: { cancel: () => void } | null = null

  constructor(readonly cfg: HaConfig) {}

  get on(): boolean {
    return this.cfg.token.length > 0
  }

  /** The bridge of this engine's session, started on first use. */
  async session(host: Host): Promise<SessionBridge> {
    const id = await host.sessionId()
    if (this.current && this.current.sessionId === id) return this.current
    if (this.current) await this.current.end(host, 'replaced')
    const instance = await this.instanceId(host)
    const session = new SessionBridge(instance, id, this.cfg)
    this.current = session
    const cwd = await quietly(host, () => host.cwd())
    session.cwd = cwd ?? ''
    session.name = this.cfg.sessionName || baseName(session.cwd) || id.slice(0, 8)
    session.model = (await quietly(host, () => host.model())) ?? ''
    if (!this.timer) {
      this.timer = host.every(POLL_MS, () => {
        void this.current?.tick(host)
      })
    }
    return session
  }

  /** The id of this computer's installation of the mod, kept in the plugin's store across sessions. */
  private async instanceId(host: Host): Promise<string> {
    if (this.instance) return this.instance
    const stored = await quietly(host, () => host.storeGet(INSTANCE_KEY))
    if (typeof stored === 'string' && stored.length > 0) {
      this.instance = stored
      return stored
    }
    const fresh = Math.random().toString(36).slice(2, 10)
    await quietly(host, () => host.storeSet(INSTANCE_KEY, fresh))
    this.instance = fresh
    return fresh
  }

  /** The current session's bridge without starting one, for hooks that only clean up. */
  peek(): SessionBridge | null {
    return this.current
  }
}
