import { expect, mock, test } from 'claude-code/testing'
import type { TestBody } from 'claude-code/testing'

import { backgroundStart, endedTasks, stoppedTask } from '../hooks/background'
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
  on('ui.log', () => ({ value: undefined }))
  on('ui.toast', () => ({ value: undefined }))
  on('session.id', () => ({ value: 'session-aaaa' }))
  on('session.cwd', () => ({ value: '/home/me/projects/garden' }))
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
