/** What runs in the background of a session: a shell, an agent, a monitor or a workflow. */
export type BackgroundKind = 'shell' | 'agent' | 'monitor' | 'workflow'

export type BackgroundStart = { id: string; kind: BackgroundKind; title: string }

/** At most this many tasks per session are reported; the oldest give way. */
export const MAX_TASKS = 8
/**
 * A task not heard of for this long is taken as gone (its end was missed, say by a reload of the mod). The last net:
 * an agent's turn, the engine's agent list and a Stop's list of work in flight end most tasks long before.
 */
export const TASK_MAX_AGE_MS = 24 * 3600_000

const TITLE_MAX = 80
const NOTE_MAX = 80
/** The highest step count and tool-call count reported, as the integration bounds them too. */
export const PROGRESS_MAX = 10_000
export const CALLS_MAX = 1_000_000

function oneLine(text: string, limit = TITLE_MAX): string {
  const line = text.replace(/\s+/g, ' ').trim()
  return line.length > limit ? `${line.slice(0, limit - 1)}…` : line
}

/**
 * The background task a finished tool call started, from its input and result; null when it started none. A shell
 * run with `run_in_background` (or moved there by hand) names its task in `backgroundTaskId`, a background agent
 * answers `async_launched` with its `agentId`, a monitor and a workflow name theirs in `taskId`.
 */
export function backgroundStart(tool: string, input: unknown, result: unknown): BackgroundStart | null {
  const i = (input ?? {}) as Record<string, unknown>
  const r = (result ?? {}) as Record<string, unknown>
  const str = (v: unknown) => (typeof v === 'string' && v.trim() ? v : undefined)
  if (tool === 'Bash' || tool === 'PowerShell') {
    const id = str(r.backgroundTaskId)
    if (!id) return null
    return { id, kind: 'shell', title: oneLine(str(i.description) ?? str(i.command) ?? tool) }
  }
  if (tool === 'Agent' || tool === 'Task') {
    const id = str(r.agentId)
    if (r.status !== 'async_launched' || !id) return null
    return { id, kind: 'agent', title: oneLine(str(r.description) ?? str(i.description) ?? 'Agent') }
  }
  if (tool === 'Monitor') {
    const id = str(r.taskId)
    if (!id) return null
    return { id, kind: 'monitor', title: oneLine(str(i.description) ?? str(i.command) ?? 'Monitor') }
  }
  if (tool === 'Workflow') {
    const id = str(r.taskId)
    if (!id) return null
    return { id, kind: 'workflow', title: oneLine(str(r.workflowName) ?? str(i.name) ?? 'Workflow') }
  }
  return null
}

/** The task a TaskStop call stopped, or null. */
export function stoppedTask(tool: string, input: unknown, result: unknown): string | null {
  if (tool !== 'TaskStop' && tool !== 'KillShell') return null
  const i = (input ?? {}) as Record<string, unknown>
  const r = (result ?? {}) as Record<string, unknown>
  for (const v of [r.task_id, i.task_id, i.shell_id]) if (typeof v === 'string' && v) return v
  return null
}

/**
 * The tasks a task notification says are over: every `<task-id>` with a final `<status>`. A monitor's event carries
 * no final status, so its task stays. One prompt may carry several notifications.
 */
const FINAL = new Set(['completed', 'failed', 'killed', 'stopped', 'cancelled', 'canceled', 'error', 'timeout'])

export function endedTasks(text: string): string[] {
  const ended: string[] = []
  for (const block of text.split(/<\/task-notification>/)) {
    const id = /<task-id>\s*([^<\s]+)\s*<\/task-id>/.exec(block)?.[1]
    const status = /<status>\s*([^<\s]+)\s*<\/status>/.exec(block)?.[1]
    if (id && status && FINAL.has(status.toLowerCase())) ended.push(id)
  }
  return ended
}

/**
 * The ids a classic Stop names as still in flight (`background_tasks`), or null when it names no list: an engine that
 * sends none says nothing about what ended.
 */
export function runningTasks(backgroundTasks: unknown): Set<string> | null {
  if (!Array.isArray(backgroundTasks)) return null
  const ids = backgroundTasks.map(t => (t as { id?: unknown } | null)?.id)
  return new Set(ids.filter((id): id is string => typeof id === 'string' && id.length > 0))
}

/** The `$.agent.list()` statuses of an agent whose loop is over. */
export const AGENT_OVER = new Set(['completed', 'failed', 'killed'])

/** How far a background agent is, as its own step reports say. */
export type Progress = { done: number; total: number; note?: string }

/** The tools an agent reports its steps with; both take `{ done, total, note? }`. */
export const PROGRESS_TOOLS = new Set(['mcp__clawd-hq__step', 'mcp__savvy-progress__step'])

/** The progress a step call reports, or null for another tool or a report that makes no sense (it would draw a wrong bar). */
export function progressReport(tool: string, input: unknown): Progress | null {
  if (!PROGRESS_TOOLS.has(tool)) return null
  const i = (input ?? {}) as Record<string, unknown>
  if (typeof i.done !== 'number' || typeof i.total !== 'number') return null
  if (!Number.isInteger(i.done) || !Number.isInteger(i.total)) return null
  const done = Math.min(i.done, PROGRESS_MAX)
  const total = Math.min(i.total, PROGRESS_MAX)
  if (total < 1 || done < 0 || done > total) return null
  const note = typeof i.note === 'string' ? oneLine(i.note, NOTE_MAX) : ''
  return note ? { done, total, note } : { done, total }
}
