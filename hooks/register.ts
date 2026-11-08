import type { Register } from 'claude-code'

import { backgroundStart, endedTasks, PROGRESS_TOOLS, progressReport, runningTasks, stoppedTask } from './background'
import { readConfig } from './ha'
import type { Host } from './host'
import { nonce, toToolAnswers, type ToolAnswers } from './questions'
import { Bridge, type SessionBridge } from './session'

/**
 * In bypassPermissions the Desktop app answers an AskUserQuestion by itself after a few seconds. A question that is
 * still open after this long is a real one (a terminal shows the dialog there) and goes to the screens; one answered
 * before it was never announced.
 */
export const BYPASS_QUESTION_GRACE_MS = 10_000

/**
 * tessera-bridge: this Claude session on the Tessera screens, through the tessera_claude integration in Home
 * Assistant. Every session that loads the mod reports itself (state, context, the account's limits, its open
 * questions) under its own id; the integration keeps the registry, the display plugin shows it and answers.
 *
 * The engine is bound once, in session.start, as a Host of closures (each spelled `$.noun.event(...)` there), and
 * every later hook and timer uses that: the engine refuses a module that passes `$` itself around.
 */
export const register: Register = (on, options) => {
  const cfg = readConfig(options)
  const bridge = new Bridge(cfg)
  let host: Host | null = null

  const toolResult = (questions: unknown, answers: ToolAnswers) => ({ result: { questions, answers } })

  async function pushUsage(h: Host, session: SessionBridge) {
    try {
      const usage = await h.usage()
      session.limits = usage.rateLimits
      if (usage.rateLimits.length) session.limitsAt = await h.now()
      session.context = usage.context
    } catch {
      // No usage yet: the first measurement brings it.
    }
    session.touch()
    await session.report(h)
  }

  on('session.start', async ($, e, next) => {
    const started = await next(e)
    if (bridge.on) {
      host = {
        fetch: (url, init) => $.http.fetch(url, init),
        now: () => $.clock.now(),
        after: (ms, fn) => $.clock.after(ms, fn),
        every: (ms, fn) => $.clock.every(ms, fn),
        log: text => $.ui.log(text, { to: 'debug' }),
        toast: text => $.ui.toast(text),
        storeGet: key => $.store.get(key),
        storeSet: (key, value) => $.store.set(key, value),
        sessionId: () => $.session.id(),
        cwd: () => $.session.cwd(),
        model: () => $.session.model(),
        usage: () => $.session.usage(),
        agents: () => $.agent.list(),
      }
      const session = await bridge.session(host)
      await pushUsage(host, session)
    }
    return started
  })

  on('session.measure', async ($, e, next) => {
    if (host) {
      const session = await bridge.session(host)
      if (e.changed.includes('rateLimits')) {
        session.limits = e.rateLimits
        if (e.rateLimits.length) session.limitsAt = await host.now()
      }
      if (e.changed.includes('context')) session.context = e.context
      session.touch()
    }
    return next(e)
  })

  // A subagent's turn starts and ends here too (`agentId`): only the main loop's makes the session work.
  on('turn.start', async ($, e, next) => {
    if (host && e.agentId === undefined) {
      const session = await bridge.session(host)
      session.working = true
      await session.refreshName(host)
      session.touch()
    }
    return next(e)
  })

  // The classic hooks name the session's permission mode (`default`, `auto`, `bypassPermissions`, ...); the bridge
  // keeps the last word, so it knows whether anybody is asked at all.
  const takeMode = async (mode: unknown) => {
    if (host && typeof mode === 'string' && mode) (await bridge.session(host)).mode = mode
  }
  on('classic.UserPromptSubmit', async ($, e, next) => {
    await takeMode(e.permission_mode)
    return next(e)
  }).catch(($, e, next) => next(e))
  // A background agent's step report moves its task's bar. The session is only looked up (`peek`): a subagent's loop
  // must not start one. Two hooks hear the same report, as a hook above ours may keep the other from it; twice is harmless.
  const takeProgress = (tool: string, input: unknown, agentId: unknown) => {
    const progress = progressReport(tool, input)
    if (!host || !progress || typeof agentId !== 'string' || !agentId) return
    if (bridge.peek()?.progressTask(agentId, progress)) host.log(`tessera-bridge: progress of ${agentId} ${progress.done}/${progress.total}`)
  }
  on('classic.PostToolUse', async ($, e, next) => {
    await takeMode(e.permission_mode)
    takeProgress(e.tool_name, e.tool_input, e.agent_id)
    return next(e)
  }).catch(($, e, next) => next(e))
  // The source that works in a real session: the model's response of a subagent's loop. A step tool is served inside
  // its owner's own tool.call hook, which answers without next, so neither the tool.call hook below nor PostToolUse ever
  // hears it; the response lists the call whoever answers it. Every other call the agent asks for counts as activity,
  // the honest fallback for an agent that reports no step (its type may lack the tool).
  on('turn.step', async function* ($, e, next) {
    const step = yield* next(e)
    try {
      const session = host && e.agentId ? bridge.peek() : undefined
      if (session && e.agentId) {
        for (const use of step.toolUses) {
          if (PROGRESS_TOOLS.has(use.name)) takeProgress(use.name, use.input, e.agentId)
          else session.countCall(e.agentId)
        }
      }
    } catch {
      // The display is not worth a model response: whatever went wrong here, the step stands.
    }
    return step
  })

  on('tool.call', { tool: 'AskUserQuestion' }, async ($, e, next) => {
    const h = host
    if (!h) return next(e)
    const raw = Array.isArray(e.questions) ? (e.questions as unknown[]) : []
    if (raw.length === 0) return next(e)
    const session = await bridge.session(h)
    const id = e.tool_use_id ?? `q-${nonce()}`

    // The dialog in the app runs beneath, in parallel with the display; the integration decides who was first.
    const desktop = next(e).then(
      result => ({ from: 'desktop' as const, result, error: undefined }),
      (error: unknown) => ({ from: 'desktop' as const, result: undefined, error }),
    )
    const settle = async () => {
      const late = await desktop
      if (late.error !== undefined || late.result === undefined) throw late.error
      return late.result
    }
    if (session.mode === 'bypassPermissions') {
      // The app may answer by itself in a moment: such a question is nobody's, and the screens never hear of it.
      const grace = new Promise<boolean>(resolve => h.after(BYPASS_QUESTION_GRACE_MS, () => resolve(false)))
      if (await Promise.race([desktop.then(() => true), grace])) return settle()
    }
    const question = await session.ask(h, id, raw)
    if (!question) return settle()

    const display = session.waitFor(id).then(status => ({ from: 'display' as const, status }))
    const first = await Promise.race([desktop, display])

    if (first.from === 'display') {
      const status = first.status
      const answers = status.status === 'answered' && status.answer ? toToolAnswers(question, status.answer) : null
      if (answers) {
        h.toast('Antwort vom Tessera-Screen übernommen')
        return toolResult(e.questions, answers)
      }
      // Cancelled, or an answer that does not fit: the dialog in the app stands.
      return settle()
    }

    // The app settled first: the PC's answer stands unless the display was quicker at the integration.
    const refused = first.error !== undefined || first.result?.deny !== undefined || first.result?.isError === true
    const claim = await session.claim(h, id, refused ? 'cancelled' : 'answered')
    session.settleQuestion(id, { status: refused ? 'cancelled' : 'answered', by: 'pc' })
    if (claim && !claim.accepted && claim.by === 'display' && claim.answer) {
      const answers = toToolAnswers(question, claim.answer)
      if (answers) {
        h.toast('Antwort vom Tessera-Screen übernommen')
        return toolResult(e.questions, answers)
      }
    }
    if (first.error !== undefined || first.result === undefined) throw first.error
    return first.result
  }).catch(($, e, next) => next(e))

  on('tool.check', async ($, e, next) => {
    const verdict = await next(e)
    const h = host
    const id = e.tool_use_id
    // A newer engine names who answers the ask (`approver`); only a person's ask is a hint. `decider: "host"` is
    // not excluded: the Desktop app hosts the session and the person still answers there.
    const approver = (e as { approver?: string }).approver
    if (
      h &&
      cfg.permissionHints &&
      verdict.decision === 'ask' &&
      (approver === undefined || approver === 'person') &&
      id !== undefined &&
      e.tool !== 'AskUserQuestion'
    ) {
      const session = await bridge.session(h)
      // A hint only after the delay, and only for an ask a person sees: in `auto` the classifier answers most asks
      // by itself, and the delay starts with the PermissionRequest that hands one to the person.
      session.askPermission(id, e.tool, e.input)
      h.after(cfg.hintDelayMs, () => {
        if (session.hintIfStillOpen(id)) void session.report(h)
      })
    }
    return verdict
  }).catch(($, e, next) => next(e))

  // The engine is about to ask a person about a call: from now on its ask is a prompt, and a hint after the delay.
  on('classic.PermissionRequest', async ($, e, next) => {
    const h = host
    if (h) {
      await takeMode(e.permission_mode)
      const session = await bridge.session(h)
      const id = session.prompted(e.tool_name, e.tool_input)
      if (id !== null) {
        h.after(cfg.hintDelayMs, () => {
          if (session.hintIfStillOpen(id)) void session.report(h)
        })
      }
    }
    return next(e)
  }).catch(($, e, next) => next(e))

  // Once a tool ran (or was refused) the person has answered on the PC: the hint of that call goes. A call that
  // started a background task (or stopped one) changes the session's list of them.
  on('tool.call', async ($, e, next) => {
    const ran = await next(e)
    const h = host
    if (h && e.tool_use_id !== undefined) bridge.peek()?.settleAsk(e.tool_use_id)
    if (ran.result !== undefined && ran.isError !== true) takeProgress(e.tool, e, (e as { agentId?: string }).agentId)
    if (h && ran.result !== undefined && ran.isError !== true) {
      const started = backgroundStart(e.tool, e, ran.result)
      const stopped = stoppedTask(e.tool, e, ran.result)
      if (started || stopped) {
        const session = await bridge.session(h)
        if (started) session.startTask(started, await h.now())
        if (stopped) session.endTask(stopped)
      }
    }
    return ran
  }).catch(($, e, next) => next(e))

  // A background task's notification says it is over (completed, failed, stopped).
  on('prompt.submit', async ($, e, next) => {
    if (host && e.origin.kind === 'task-notification') {
      const session = bridge.peek()
      for (const id of endedTasks(e.text)) session?.endTask(id)
    }
    return next(e)
  }).catch(($, e, next) => next(e))

  // The end of the main turn names the background work still in flight: a task it named before and names no more is
  // over, whether or not its notification came our way.
  on('classic.Stop', async ($, e, next) => {
    const running = runningTasks(e.background_tasks)
    if (host && running) bridge.peek()?.sweepTasks('stop', running)
    return next(e)
  }).catch(($, e, next) => next(e))

  on('turn.complete', async ($, e, next) => {
    if (host && e.agentId !== undefined) {
      // A subagent's turn may end while it waits or resumes under the same id: its task ends only when the engine's
      // agent list says it is over (or drops it); otherwise the tick's sweep does it later. The main loop's questions
      // and hints are not this turn's.
      await bridge.peek()?.sweepAgents(host, e.agentId)
    } else if (host) {
      // The turn ended, or was aborted: only this session's questions and hints are over.
      const session = await bridge.session(host)
      session.settleAll()
      await session.report(host)
    }
    return next(e)
  })

  on('session.end', async ($, e, next) => {
    if (host) await bridge.peek()?.end(host, e.reason)
    return next(e)
  })
}
