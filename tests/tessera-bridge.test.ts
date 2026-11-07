import type { AgentInfo, AgentStatus } from 'claude-code'
import { expect, mock, test } from 'claude-code/testing'
import type { TestBody } from 'claude-code/testing'

import { backgroundStart, endedTasks, progressReport, stoppedTask } from '../hooks/background'
import { baseName, describeCall } from '../hooks/describe'
import { toReported, toToolAnswers } from '../hooks/questions'

const OPTIONS = {
  haUrl: 'http://ha.test:8123',
  haToken: 'test-token',
  permissionHints: 'on',
  hintDelay: '5',
}

type Call = { method: string; path: string; body: Record<string, unknown> }
type Status = { status: string; by?: string; answer?: Record<string, string[]>; nonce?: string }

/**
 * Answers the integration's API from memory and records every call. `ha.questions` is what /report answers for each
 * question id; `ha.claim` what /claim answers; `ha.down` makes every call fail.
 */
function mockHa(on: Parameters<TestBody>[1]) {
  const calls: Call[] = []
  const ha = {
    calls,
    questions: {} as Record<string, Status>,
    claim: { accepted: true } as Record<string, unknown>,
    down: false,
    /** Every line the mod sent to ui.log. */
    logs: [] as string[],
    cwd: '/home/me/projects/garden',
    reports: () => calls.filter(c => c.path === '/api/tessera_claude/v1/report').map(c => c.body),
    lastReport: () => {
      const all = ha.reports()
      return all[all.length - 1]
    },
  }
  on('http.fetch', (_, e) => {
    const init = e.init ?? {}
    const path = e.url.replace('http://ha.test:8123', '')
    const body = init.body ? (JSON.parse(init.body) as Record<string, unknown>) : {}
    calls.push({ method: init.method ?? 'GET', path, body })
    if (ha.down) return { value: { status: 503, ok: false, headers: {}, text: '' } }
    let reply: unknown = { ok: true }
    if (path.endsWith('/report')) {
      const statuses: Record<string, Status> = {}
      for (const q of (body.questions as Array<{ id: string; nonce: string }>) ?? []) {
        statuses[q.id] = ha.questions[q.id] ?? { status: 'open' }
      }
      reply = { ok: true, handle: 's1', questions: statuses }
    } else if (path.endsWith('/claim')) {
      reply = ha.claim
    }
    return { value: { status: 200, ok: true, headers: {}, text: JSON.stringify(reply) } }
  })
  on('session.start', (_, e) => ({ cwd: e.cwd }))
  on('ui.log', (_, e) => {
    ha.logs.push(String((e as { text?: unknown }).text ?? ''))
    return { value: undefined }
  })
  on('ui.toast', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'session-aaaa' }))
  on('session.cwd', () => ({ value: ha.cwd }))
  on('session.model', () => ({ value: 'claude' }))
  on('session.usage', () => ({ value: { context: { window: 1_000_000, tokens: 50_000, percent: 5 }, rateLimits: [] } }))
  mock.store(on, {})
  return ha
}

const TWO = [
  { question: 'Weiter machen?', header: 'Weiter', multiSelect: false, options: [{ label: 'Ja', description: '' }, { label: 'Nein', description: '' }] },
]

/** Raises session.start, where the mod binds the engine. */
function start($: Parameters<TestBody>[0]) {
  return $.session.start({ surface: 'desktop', isInteractive: true, cwd: '/home/me/projects/garden' })
}

type Answered = { result?: { answers?: Record<string, string> }; deny?: string }

test('describeCall and baseName read well', async () => {
  expect(describeCall('Bash', { command: 'git   status' })).toBe('Bash: git status')
  expect(describeCall('mcp__ha-tools__ha_search', { query: 'lamp' })).toBe('ha_search (ha-tools): lamp')
  expect(baseName('/home/me/projects/garden/')).toBe('garden')
  expect(baseName('C:\\work\\shop')).toBe('shop')
})

test('a question gets stable part and option ids; labels are only for showing', async () => {
  const q = toReported('toolu_1', [
    { question: 'Welche Farbe?', header: 'Farbe', multiSelect: true, options: [{ label: 'Rot', description: 'warm' }, { label: 'Blau', description: '' }, { label: 'Rot', description: 'again' }] },
    { question: 'Weiter?', header: 'Weiter', multiSelect: false, options: [{ label: 'Ja', description: '' }, { label: 'Nein', description: '' }] },
  ])
  expect(q?.parts.map(p => p.id)).toEqual(['p0', 'p1'])
  expect(q?.parts[0]?.options.map(o => o.id)).toEqual(['o0', 'o1', 'o2'])
  // Two options with the same label stay apart by id; a multi-select joins its labels with a comma.
  expect(toToolAnswers(q!, { p0: ['o0', 'o2'], p1: ['o1'] })).toEqual({ 'Welche Farbe?': 'Rot, Rot', 'Weiter?': 'Nein' })
  // Refused: a part without an answer, an option that is not there, two answers to a single choice.
  expect(toToolAnswers(q!, { p0: ['o0'] })).toBe(null)
  expect(toToolAnswers(q!, { p0: ['o0'], p1: ['o9'] })).toBe(null)
  expect(toToolAnswers(q!, { p0: ['o0'], p1: ['o0', 'o1'] })).toBe(null)
})

test('a question answered on the display is answered in the app, by option id', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  let dialogShown = false
  on('tool.call', { tool: 'AskUserQuestion' }, () => {
    dialogShown = true
    return new Promise(() => {}) // the app never answers in this test: the display does
  })

  await start($)
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q1' })
  await clock.settle()

  const reported = ha.lastReport()
  expect(reported?.session).toBe('session-aaaa')
  expect(reported?.state).toBe('asking')
  const questions = reported?.questions as Array<{ id: string; nonce: string; parts: Array<{ options: Array<{ id: string }> }> }>
  expect(questions[0]?.id).toBe('toolu_q1')
  expect(questions[0]?.parts[0]?.options.map(o => o.id)).toEqual(['o0', 'o1'])

  // Minutes pass while nobody answers: the poll keeps running, nothing breaks. Then the display answers.
  await clock.advance(120_000)
  ha.questions['toolu_q1'] = { status: 'answered', by: 'display', answer: { p0: ['o1'] }, nonce: questions[0]!.nonce }
  await clock.advance(2_000)

  const result = (await call) as Answered
  expect(dialogShown).toBe(true)
  expect(result.result?.answers).toEqual({ 'Weiter machen?': 'Nein' })
  // The question is gone from the next report.
  await clock.advance(2_000)
  expect((ha.lastReport()?.questions as unknown[]).length).toBe(0)
  expect(ha.lastReport()?.state).toBe('idle')
})

test('an answer from before a reload does not fit a question registered after it', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(() => {}))
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q1' })
  await clock.settle()
  // An answer that carries another nonce: not this question's.
  ha.questions['toolu_q1'] = { status: 'answered', by: 'display', answer: { p0: ['o0'] }, nonce: 'stale' }
  await clock.advance(5_000)
  let settled = false
  void call.then(() => { settled = true }, () => { settled = true })
  await clock.settle()
  expect(settled).toBe(false)
})

test('an answer in the app is claimed at the integration and stands', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({ result: { questions: [], answers: { 'Weiter machen?': 'Ja' } } }))

  await start($)
  const result = (await $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q2' })) as Answered
  await clock.advance(10)

  expect(result.result?.answers).toEqual({ 'Weiter machen?': 'Ja' })
  const claim = ha.calls.find(c => c.path.endsWith('/claim'))
  expect(claim?.body.id).toBe('toolu_q2')
  expect(claim?.body.by).toBe('pc')
  expect(claim?.body.outcome).toBe('answered')
})

test('when the display was quicker at the integration, its answer is the one that counts', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  ha.claim = { accepted: false, status: 'answered', by: 'display', answer: { p0: ['o1'] } }
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({ result: { questions: [], answers: { 'Weiter machen?': 'Ja' } } }))

  await start($)
  const result = (await $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q3' })) as Answered
  await clock.advance(10)
  expect(result.result?.answers).toEqual({ 'Weiter machen?': 'Nein' })
})

test('a multi-select and several parts come back comma-joined, keyed by the question text', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(() => {}))
  const questions = [
    { question: 'Welche Module?', header: 'Module', multiSelect: true, options: [{ label: 'A', description: '' }, { label: 'B', description: '' }, { label: 'C', description: '' }] },
    { question: 'Jetzt bauen?', header: 'Bauen', multiSelect: false, options: [{ label: 'Ja', description: '' }, { label: 'Später', description: '' }] },
  ]
  await start($)
  const call = $.tool.call({ tool: 'AskUserQuestion', questions, tool_use_id: 'toolu_q4' })
  await clock.settle()
  const nonce = (ha.lastReport()?.questions as Array<{ nonce: string }>)[0]!.nonce
  ha.questions['toolu_q4'] = { status: 'answered', by: 'display', answer: { p0: ['o0', 'o2'], p1: ['o1'] }, nonce }
  await clock.advance(2_000)
  const result = (await call) as Answered
  expect(result.result?.answers).toEqual({ 'Welche Module?': 'A, C', 'Jetzt bauen?': 'Später' })
})

test('a Home Assistant that is down leaves the dialog alone', { options: OPTIONS }, async ($, on) => {
  mock.clock(on)
  const ha = mockHa(on)
  ha.down = true
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({ result: { questions: [], answers: { Q: 'A' } } }))

  await start($)
  const result = (await $.tool.call({
    tool: 'AskUserQuestion',
    questions: [{ question: 'Q', header: 'H', multiSelect: false, options: [{ label: 'A', description: '' }, { label: 'B', description: '' }] }],
    tool_use_id: 'toolu_q5',
  })) as Answered
  expect(result.result?.answers).toEqual({ Q: 'A' })
})

test('the end of the turn cancels this session\'s question and nothing else', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({ result: { questions: [], answers: { 'Weiter machen?': 'Ja' } } }))
  on('turn.complete', () => ({ text: '' }))

  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q6' })
  await clock.advance(10)
  await start($)
  await $.turn.complete({ answer: '', durationMs: 1, isAborted: true, turnId: 'turn-1', reason: 'aborted' })
  await call
  await clock.advance(10)
  const last = ha.lastReport()
  expect((last?.questions as unknown[]).length).toBe(0)
  expect(last?.state).toBe('idle')
  // Every report names this session: the integration can never mistake it for another one's.
  for (const r of ha.reports()) expect(r.session).toBe('session-aaaa')
})

test('a measurement reports context and the account limits apart, under this session', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on, { now: Date.parse('2026-01-01T00:00:00Z') })
  const ha = mockHa(on)
  on('session.measure', (_, e) => ({ changed: e.changed, cwd: '/' }))

  await start($)
  await $.session.measure({
    context: { window: 1_000_000, tokens: 110_000, percent: 11 },
    rateLimits: [
      { kind: 'five_hour', percentUsed: 44, resetsAt: '2026-01-01T01:09:00Z' },
      { kind: 'seven_day', percentUsed: 11.5, resetsAt: '2026-01-07T00:00:00Z' },
    ],
    changed: ['rateLimits', 'context'],
  })
  await clock.advance(2_000)
  const last = ha.lastReport()
  expect(last?.context).toEqual({ percent: 11, tokens: 110_000, window: 1_000_000 })
  expect(last?.limits).toEqual([
    { kind: 'five_hour', percent: 44, resets_at: '2026-01-01T01:09:00Z' },
    { kind: 'seven_day', percent: 11.5, resets_at: '2026-01-07T00:00:00Z' },
  ])
  expect(last?.limits_age).toBe(2)
  expect(last?.instance).toBeDefined()
})

test('a permission ask that stays open is reported as waiting for approval; the tool running takes it down', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false } }))

  await start($)
  const verdict = await $.tool.check({ tool: 'Bash', input: { command: 'rm -rf build' }, tool_use_id: 't1' })
  expect(verdict.decision).toBe('ask')
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).not.toBe('permission')

  await clock.advance(4_000)
  expect(ha.lastReport()?.state).toBe('permission')
  expect(String(ha.lastReport()?.detail).startsWith('Bash: rm -rf build')).toBe(true)

  await $.tool.call({ tool: 'Bash', command: 'ls', tool_use_id: 't1' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).toBe('idle')
})

test('an ask that settles quickly is never reported as a hint', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false } }))

  await start($)
  await $.tool.check({ tool: 'Bash', input: { command: 'ls' }, tool_use_id: 't2' })
  await $.tool.call({ tool: 'Bash', command: 'ls', tool_use_id: 't2' })
  await clock.advance(10_000)
  expect(ha.reports().some(r => r.state === 'permission')).toBe(false)
})

test('the session says goodbye once, and reports nothing after it', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('session.measure', (_, e) => ({ changed: e.changed, cwd: '/' }))
  on('session.end', (_, e) => ({ sessionId: e.sessionId }))
  await start($)
  await $.session.measure({ context: { window: 1, tokens: 0, percent: 0 }, rateLimits: [], changed: ['context'] })
  await clock.advance(2_000)
  await $.session.end({ reason: 'prompt_input_exit', sessionId: 'session-aaaa', resume: { id: 'session-aaaa' } })
  const before = ha.reports().length
  expect(ha.calls.filter(c => c.path.endsWith('/end')).length).toBe(1)
  await clock.advance(60_000)
  expect(ha.reports().length).toBe(before)
})

test('background starts, stops and ends are read from the tools and the notifications', async () => {
  expect(backgroundStart('Bash', { command: 'npm   run build', run_in_background: true }, { backgroundTaskId: 'b1' })).toEqual({
    id: 'b1',
    kind: 'shell',
    title: 'npm run build',
  })
  expect(backgroundStart('Bash', { command: 'ls', description: 'List files' }, { stdout: '' })).toBe(null)
  expect(backgroundStart('Agent', { description: 'x' }, { status: 'async_launched', agentId: 'a1', description: 'Review the diff' })).toEqual({
    id: 'a1',
    kind: 'agent',
    title: 'Review the diff',
  })
  expect(backgroundStart('Agent', {}, { status: 'completed', agentId: 'a1' })).toBe(null)
  expect(backgroundStart('Monitor', { description: 'Watch CI' }, { taskId: 'm1' })?.kind).toBe('monitor')
  expect(backgroundStart('Workflow', {}, { status: 'async_launched', taskId: 'w1', workflowName: 'review' })?.title).toBe('review')
  expect(stoppedTask('TaskStop', { task_id: 'b1' }, { task_id: 'b1', task_type: 'local_bash' })).toBe('b1')
  expect(stoppedTask('Bash', {}, {})).toBe(null)
  const two =
    '<task-notification><task-id>b1</task-id><status>completed</status></task-notification>\n' +
    '<task-notification><task-id>m1</task-id><event>line</event></task-notification>' +
    '<task-notification><task-id>a1</task-id><status>failed</status></task-notification>'
  expect(endedTasks(two)).toEqual(['b1', 'a1'])
})

test('a background shell is reported with its age until TaskStop ends it', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'bg1' } }))
  on('tool.call', { tool: 'TaskStop' }, () => ({ result: { message: 'stopped', task_id: 'bg1', task_type: 'local_bash' } }))

  await start($)
  await $.tool.call({ tool: 'Bash', command: 'esphome run screen.yaml', description: 'Build the firmware', run_in_background: true, tool_use_id: 't9' })
  await clock.advance(2_000)
  const tasks = ha.lastReport()?.tasks as Array<{ id: string; kind: string; title: string; age: number }>
  expect(tasks.map(t => [t.id, t.kind, t.title])).toEqual([['bg1', 'shell', 'Build the firmware']])

  await clock.advance(60_000)
  const later = ha.lastReport()?.tasks as Array<{ age: number }>
  expect(later[0]!.age >= 60).toBe(true)

  await $.tool.call({ tool: 'TaskStop', task_id: 'bg1', tool_use_id: 't10' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.tasks).toEqual([])
})

test('progress is read from the step tools and nowhere else', () => {
  expect(progressReport('mcp__clawd-hq__step', { done: 2, total: 5, note: '  Writing' + String.fromCharCode(10) + '  tests ' })).toEqual({
    done: 2,
    total: 5,
    note: 'Writing tests',
  })
  expect(progressReport('mcp__savvy-progress__step', { done: 0, total: 1 })).toEqual({ done: 0, total: 1 })
  expect(progressReport('mcp__clawd-hq__step', { done: 3, total: 3, note: '   ' })).toEqual({ done: 3, total: 3 })
  expect(progressReport('mcp__clawd-hq__step', { done: 6, total: 5 })).toBe(null)
  expect(progressReport('mcp__clawd-hq__step', { done: 1.5, total: 5 })).toBe(null)
  expect(progressReport('mcp__clawd-hq__step', { done: 1, total: 0 })).toBe(null)
  expect(progressReport('mcp__clawd-hq__step', { done: -1, total: 5 })).toBe(null)
  expect(progressReport('mcp__clawd-hq__step', { done: '2', total: 5 })).toBe(null)
  expect(progressReport('mcp__other__step', { done: 1, total: 5 })).toBe(null)
  const cut = progressReport('mcp__clawd-hq__step', { done: 1, total: 2, note: 'x'.repeat(200) })
  expect(cut?.note?.length).toBe(80)
})

const agentInfo = (id: string, status: AgentStatus): AgentInfo => ({ id, status, description: '', type: 'general-purpose' })

/** A session with one background agent `a1` running, and the step tool answered by a hook beneath ours. */
async function withAgent($: Parameters<TestBody>[0], on: Parameters<TestBody>[1]) {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.call', { tool: 'Agent' }, () => ({ result: { status: 'async_launched', agentId: 'a1', description: 'Review' } }))
  on('tool.call', { tool: 'mcp__clawd-hq__step' }, () => ({ result: {} }))
  on('prompt.submit', (_, e) => ({ text: e.text }))
  // What the engine's agent list says; a test sets `agents.list`.
  const agents = { list: [agentInfo('a1', 'running')] }
  on('agent.list', () => ({ value: agents.list }))
  await start($)
  await $.tool.call({ tool: 'Agent', description: 'Review', run_in_background: true, tool_use_id: 't11' })
  await clock.advance(2_000)
  const task = () => (ha.lastReport()?.tasks as Array<Record<string, unknown>>)[0]!
  return { clock, ha, task, agents }
}

test("a background agent's progress comes from the step tool of its loop", { options: OPTIONS }, async ($, on) => {
  const { clock, ha, task } = await withAgent($, on)
  expect(task().total).toBe(undefined)

  await $.tool.call({ tool: 'mcp__clawd-hq__step', done: 2, total: 5, note: 'Writing tests', agentId: 'a1', tool_use_id: 't12' })
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([2, 5, 'Writing tests'])

  // Another loop's steps, or the main loop's, are not this task's.
  await $.tool.call({ tool: 'mcp__clawd-hq__step', done: 4, total: 5, agentId: 'zz', tool_use_id: 't13' })
  await $.tool.call({ tool: 'mcp__clawd-hq__step', done: 4, total: 5, tool_use_id: 't14' })
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([2, 5, 'Writing tests'])

  await $.prompt.submit({
    text: '<task-notification><task-id>a1</task-id><status>completed</status></task-notification>',
    origin: { kind: 'task-notification' },
  })
  await clock.advance(2_000)
  expect(ha.lastReport()?.tasks).toEqual([])
})

test("a background agent's progress also comes from the classic hook of its loop", { options: OPTIONS }, async ($, on) => {
  const { clock, task } = await withAgent($, on)
  const step = { tool_name: 'mcp__savvy-progress__step', tool_input: { done: 1, total: 4 }, tool_response: {}, tool_use_id: 't15' }
  await $.classic.PostToolUse({ ...step, agent_id: 'a1' })
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([1, 4, ''])

  await $.classic.PostToolUse({ ...step, tool_input: { done: 3, total: 4 }, agent_id: 'zz' })
  await $.classic.PostToolUse({ ...step, tool_input: { done: 3, total: 4 } })
  await clock.advance(2_000)
  expect(task().done).toBe(1)
})

test("a background agent's task ends with its loop's turn, and leaves the main loop's question be", { options: OPTIONS }, async ($, on) => {
  on('turn.complete', () => ({ text: '' }))
  let answer: ((value: unknown) => void) | null = null
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(resolve => { answer = resolve }))
  const { clock, ha, agents } = await withAgent($, on)
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q10' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).toBe('asking')

  // No notification comes: the end of the agent's run, with the list saying it is over, is the end of its task.
  agents.list = [agentInfo('a1', 'completed')]
  await $.turn.complete({ answer: 'done', durationMs: 1, isAborted: false, turnId: 'turn-a1', reason: 'answer', agentId: 'a1' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.tasks).toEqual([])
  expect(ha.lastReport()?.state).toBe('asking')
  expect((ha.lastReport()?.questions as unknown[]).length).toBe(1)

  answer!({ result: { questions: [], answers: {} } })
  await call
})

const turnDone = (agentId?: string) => ({ answer: 'done', durationMs: 1, isAborted: false, turnId: 'turn-x', reason: 'answer' as const, agentId })

test("a subagent's turn.start does not make the main session work", { options: OPTIONS }, async ($, on) => {
  on('turn.start', (_, e) => ({ turnId: e.turnId }))
  const { clock, ha } = await withAgent($, on)
  await $.turn.start({ text: 'go', turnId: 'turn-a1', agentId: 'a1' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).toBe('idle')
})

test("a subagent's turn.complete leaves the working main session and its question be", { options: OPTIONS }, async ($, on) => {
  on('turn.start', (_, e) => ({ turnId: e.turnId }))
  on('turn.complete', () => ({ text: '' }))
  let answer: ((value: unknown) => void) | null = null
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(resolve => { answer = resolve }))
  const { clock, ha, agents } = await withAgent($, on)
  await $.turn.start({ text: 'go', turnId: 'turn-main' })
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q11' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).toBe('asking')

  agents.list = [agentInfo('a1', 'completed')]
  await $.turn.complete(turnDone('a1'))
  await clock.advance(2_000)
  expect(ha.lastReport()?.tasks).toEqual([])
  expect(ha.lastReport()?.state).toBe('asking')
  expect((ha.lastReport()?.questions as unknown[]).length).toBe(1)

  // The app answers in the end, so no call is left hanging.
  answer!({ result: { questions: [], answers: {} } })
  await call
})

test("an agent's turn.complete while the list shows it waiting or running keeps its task", { options: OPTIONS }, async ($, on) => {
  on('turn.complete', () => ({ text: '' }))
  const { clock, ha, agents } = await withAgent($, on)
  for (const status of ['waiting', 'running'] as const) {
    agents.list = [agentInfo('a1', status)]
    await $.turn.complete(turnDone('a1'))
    await clock.advance(2_000)
    expect(taskIds(ha)).toEqual(['a1'])
  }

  // Once the list shows it over, the next turn.complete ends it.
  agents.list = [agentInfo('a1', 'completed')]
  await $.turn.complete(turnDone('a1'))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
})

test("an agent the list no longer names ends at its turn.complete; a waiting one ends at the tick once over", { options: OPTIONS }, async ($, on) => {
  on('turn.complete', () => ({ text: '' }))
  const { clock, ha, agents } = await withAgent($, on)
  agents.list = [agentInfo('a1', 'waiting')]
  await $.turn.complete(turnDone('a1'))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1'])

  agents.list = [agentInfo('a1', 'failed')]
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
})

test("an agent the list has dropped ends at its turn.complete", { options: OPTIONS }, async ($, on) => {
  on('turn.complete', () => ({ text: '' }))
  const { clock, ha, agents } = await withAgent($, on)
  agents.list = []
  await $.turn.complete(turnDone('a1'))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
})

test('a progress report logs once, and only when it moves a known task', { options: OPTIONS }, async ($, on) => {
  const { clock, ha } = await withAgent($, on)
  const logged = () => ha.logs.filter(l => l.includes('progress of'))
  const step = { tool: 'mcp__clawd-hq__step', done: 2, total: 5, note: 'Writing', tool_use_id: 't20' }

  // An unknown agent id: nothing logged, nothing changed.
  await $.tool.call({ ...step, agentId: 'zz' })
  await $.classic.PostToolUse({ tool_name: step.tool, tool_input: step, tool_response: {}, tool_use_id: 't21', agent_id: 'zz' })
  expect(logged()).toEqual([])

  // Both hooks hear the same report: one line.
  await $.tool.call({ ...step, agentId: 'a1' })
  await $.classic.PostToolUse({ tool_name: step.tool, tool_input: step, tool_response: {}, tool_use_id: 't20', agent_id: 'a1' })
  await clock.advance(2_000)
  expect(logged()).toEqual(['tessera-bridge: progress of a1 2/5'])
})

/** The ids of the tasks the last report holds. */
function taskIds(ha: ReturnType<typeof mockHa>) {
  return (ha.lastReport()?.tasks as Array<{ id: string }>).map(t => t.id)
}

test('an agent the engine lists as over, or stops listing, leaves the report; one it never listed stays', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  let n = 0
  on('tool.call', { tool: 'Agent' }, () => ({ result: { status: 'async_launched', agentId: `a${++n}`, description: `Agent ${n}` } }))
  const info = (id: string, status: AgentStatus): AgentInfo => ({ id, status, description: '', type: 'general-purpose' })
  let listed: AgentInfo[] = []
  on('agent.list', () => ({ value: listed }))

  await start($)
  for (const id of ['t30', 't31', 't32']) await $.tool.call({ tool: 'Agent', description: 'x', run_in_background: true, tool_use_id: id })
  // Right after the launch the list may not name them yet: nothing goes.
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1', 'a2', 'a3'])

  listed = [info('a1', 'running'), info('a2', 'running')]
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1', 'a2', 'a3'])

  listed = [info('a1', 'completed')]
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a3'])
})

test("the main turn's Stop sweeps the background tasks it named before and names no more", { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('classic.Stop', () => ({}))
  let n = 0
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: `bg${++n}` } }))
  const running = (id: string) => ({ id, type: 'shell', status: 'running', description: '' })

  await start($)
  await $.tool.call({ tool: 'Bash', command: 'npm run watch', run_in_background: true, tool_use_id: 't40' })
  await $.classic.Stop({ stop_hook_active: false, background_tasks: [running('bg1')] })
  await $.tool.call({ tool: 'Bash', command: 'npm run serve', run_in_background: true, tool_use_id: 't41' })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg1', 'bg2'])

  // bg1 ended unheard; bg2 was never named (its id may be spelled otherwise there), so it stays.
  await $.classic.Stop({ stop_hook_active: false, background_tasks: [] })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg2'])

  // A Stop that names no list at all (an older engine) sweeps nothing.
  await $.classic.Stop({ stop_hook_active: false, background_tasks: [running('bg2')] })
  await $.classic.Stop({ stop_hook_active: false })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg2'])
})

/** The classic hooks' bottoms, and the session's permission mode as the next prompt names it. */
function classic(on: Parameters<TestBody>[1]) {
  on('classic.UserPromptSubmit', () => ({}))
  on('classic.PermissionRequest', () => ({}))
  on('classic.PostToolUse', () => ({}))
}

test('in auto mode an ask is a hint only once the engine asks a person (a PermissionRequest)', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false } }))

  await start($)
  await $.classic.UserPromptSubmit({ prompt: 'build it', permission_mode: 'auto' })
  await $.tool.check({ tool: 'Bash', input: { command: 'esphome run screen.yaml' }, tool_use_id: 't3' })
  // The classifier takes its time, or the call runs long: no person is asked, so nothing is a hint.
  await clock.advance(30_000)
  expect(ha.reports().some(r => r.state === 'permission')).toBe(false)

  // The classifier hands the call to the person: a hint after the delay.
  await $.classic.PermissionRequest({ tool_name: 'Bash', tool_input: { command: 'esphome run screen.yaml' }, permission_mode: 'auto' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).not.toBe('permission')
  await clock.advance(4_000)
  expect(ha.lastReport()?.state).toBe('permission')
  expect(String(ha.lastReport()?.detail).startsWith('Bash: esphome run screen.yaml')).toBe(true)

  await $.tool.call({ tool: 'Bash', command: 'esphome run screen.yaml', tool_use_id: 't3' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.state).toBe('idle')
})

test('in bypassPermissions nothing waits for a person: no ask is a hint', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))

  await start($)
  await $.classic.UserPromptSubmit({ prompt: 'go', permission_mode: 'bypassPermissions' })
  await $.tool.check({ tool: 'Bash', input: { command: 'rm -rf build' }, tool_use_id: 't4' })
  await $.classic.PermissionRequest({ tool_name: 'Bash', tool_input: { command: 'rm -rf build' }, permission_mode: 'bypassPermissions' })
  await clock.advance(30_000)
  expect(ha.reports().some(r => r.state === 'permission')).toBe(false)
})

test('in bypassPermissions a question the app answers by itself is never reported', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  let answer: ((value: unknown) => void) | null = null
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(resolve => { answer = resolve }))

  await start($)
  await $.classic.UserPromptSubmit({ prompt: 'go', permission_mode: 'bypassPermissions' })
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q8' })
  await clock.advance(4_000)
  answer!({ result: { questions: [], answers: { 'Weiter machen?': '[No preference]' } } })
  const result = (await call) as Answered
  expect(result.result?.answers).toEqual({ 'Weiter machen?': '[No preference]' })
  await clock.advance(2_000)
  expect(ha.reports().some(r => r.state === 'asking' || (r.questions as unknown[]).length > 0)).toBe(false)
  expect(ha.calls.some(c => c.path.endsWith('/claim'))).toBe(false)
})

test('in bypassPermissions a question that stays open goes to the screens after the grace', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.call', { tool: 'AskUserQuestion' }, () => new Promise(() => {}))

  await start($)
  await $.classic.UserPromptSubmit({ prompt: 'go', permission_mode: 'bypassPermissions' })
  const call = $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q9' })
  await clock.advance(8_000)
  expect(ha.reports().some(r => r.state === 'asking')).toBe(false)
  await clock.advance(4_000)
  expect(ha.lastReport()?.state).toBe('asking')
  const questions = ha.lastReport()?.questions as Array<{ id: string; nonce: string }>
  expect(questions[0]?.id).toBe('toolu_q9')

  ha.questions['toolu_q9'] = { status: 'answered', by: 'display', answer: { p0: ['o0'] }, nonce: questions[0]!.nonce }
  await clock.advance(2_000)
  const result = (await call) as Answered
  expect(result.result?.answers).toEqual({ 'Weiter machen?': 'Ja' })
})

test('the session takes the name of the folder it moved into', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('turn.start', (_, e) => ({ turnId: e.turnId }))
  ha.cwd = 'C:\\Users\\me\\.claude\\spares\\spare-4YmsPh'

  await start($)
  expect(ha.lastReport()?.name).toBe('spare-4YmsPh')
  ha.cwd = 'C:\\Users\\me\\projects\\garden'
  await $.turn.start({ text: 'hi', turnId: 'turn-2' })
  await clock.advance(2_000)
  expect(ha.lastReport()?.name).toBe('garden')
  expect(ha.lastReport()?.cwd).toBe('C:\\Users\\me\\projects\\garden')
})
