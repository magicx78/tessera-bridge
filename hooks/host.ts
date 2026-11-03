import type { HttpInit, HttpResponse, SessionUsage, TimerCall } from 'claude-code'

/**
 * The engine as a hook bound it from its `$`, each member spelled `$.noun.event(...)` there (the engine refuses a
 * module that passes `$` itself around); used by every later hook, timer and request.
 */
export type Host = {
  /** `$.http.fetch`. */
  fetch: (url: string, init?: HttpInit) => Promise<HttpResponse>
  /** `$.clock.now`. */
  now: () => Promise<number>
  /** `$.clock.after`. */
  after: TimerCall
  /** `$.clock.every`. */
  every: TimerCall
  /** `$.ui.log`, to the debug log. */
  log: (text: string) => void
  /** `$.ui.toast`. */
  toast: (text: string) => void
  /** `$.store.get`. */
  storeGet: (key: string) => Promise<unknown>
  /** `$.store.set`. */
  storeSet: (key: string, value: unknown) => Promise<void>
  /** `$.session.id`. */
  sessionId: () => Promise<string>
  /** `$.session.cwd`. */
  cwd: () => Promise<string>
  /** `$.session.model`. */
  model: () => Promise<string>
  /** `$.session.usage`. */
  usage: () => Promise<SessionUsage>
}
