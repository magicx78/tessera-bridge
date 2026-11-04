import type { Register } from 'claude-code'

import { backgroundStart, endedTasks, stoppedTask } from './background'
import { readConfig } from './ha'
import type { Host } from './host'
import { nonce, toToolAnswers, type ToolAnswers } from './questions'
import { Bridge, type SessionBridge } from './session'

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

  on('turn.start', async ($, e, next) => {
    if (host) {
      const session = await bridge.session(host)
      session.working = true
      session.touch()
    }
    return next(e)
  })

  on('tool.call', { tool: 'AskUserQuestion' }, async ($, e, next) => {
    const h = host
    if (!h) return next(e)
    const raw = Array.isArray(e.questions) ? (e.questions as unknown[]) : []
    if (raw.length === 0) return next(e)
    const session = await bridge.session(h)
    const id = e.tool_use_id ?? `q-${nonce()}`
    const question = await session.ask(h, id, raw)
    if (!question) return next(e)

    // The dialog in the app runs beneath, in parallel with the display; the integration decides who was first.
    const desktop = next(e).then(
      result => ({ from: 'desktop' as const, result, error: undefined }),
      (error: unknown) => ({ from: 'desktop' as const, result: undefined, error }),
    )
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
      const late = await desktop
      if (late.error !== undefined || late.result === undefined) throw late.error
      return late.result
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
      // Not at once: in auto mode most asks settle by themselves in a moment.
      session.askPermission(id, e.tool, e.input)
      h.after(cfg.hintDelayMs, () => {
        if (session.hintIfStillOpen(id)) void session.report(h)
      })
    }
    return verdict
  }).catch(($, e, next) => next(e))

  // Once a tool ran (or was refused) the person has answered on the PC: the hint of that call goes. A call that
  // started a background task (or stopped one) changes the session's list of them.
  on('tool.call', async ($, e, next) => {
    const ran = await next(e)
    const h = host
    if (h && e.tool_use_id !== undefined) bridge.peek()?.settleAsk(e.tool_use_id)
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

  on('turn.complete', async ($, e, next) => {
    // The turn ended, or was aborted: only this session's questions and hints are over.
    if (host) {
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
