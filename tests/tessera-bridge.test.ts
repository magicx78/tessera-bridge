import type { AgentInfo, AgentStatus, SessionAppendInput, ToolCallResult } from 'claude-code'
import { expect, mock, test } from 'claude-code/testing'
import type { TestBody } from 'claude-code/testing'

import { backgroundStart, endedTasks, progressReport, shortenCommand, stoppedTask } from '../hooks/background'
import { baseName, describeCall } from '../hooks/describe'
import { readConfig } from '../hooks/ha'
import type { Host } from '../hooks/host'
import { toReported, toToolAnswers } from '../hooks/questions'
import { POLL_MS, SessionBridge } from '../hooks/session'

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
    questions: {} as Record<string, Status | null>,
    claim: { accepted: true } as Record<string, unknown>,
    down: false,
    /** Makes `$.session.id` fail, for bookkeeping that breaks after a tool ran. */
    idFails: false,
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
      const statuses: Record<string, Status | null> = {}
      for (const q of (body.questions as Array<{ id: string; nonce: string }>) ?? []) {
        statuses[q.id] = q.id in ha.questions ? ha.questions[q.id]! : { status: 'open' }
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
  on('session.id', () => {
    if (ha.idFails) throw new Error('no session id')
    return { value: 'session-aaaa' }
  })
  on('session.cwd', () => ({ value: ha.cwd }))
  on('session.model', () => ({ value: 'claude' }))
  on('session.usage', () => ({ value: { context: { window: 1_000_000, tokens: 50_000, percent: 5 }, rateLimits: [], startedAt: 0 } }))
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

  // Time passes while nobody answers: the poll keeps running, nothing breaks. Then the display answers. Every poll
  // tick costs real event-loop time in the kit, so the wait is ten ticks (not the 80 that two minutes would be):
  // that is what kept this test near its 5 s limit, and past it under load.
  await clock.advance(10 * POLL_MS)
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

test('a shell command without a description is shortened for the screen', () => {
  const short = (c: string) => shortenCommand(c, 'Bash')
  expect(short('cd /c/Users/magic/AppData/Local/Temp/x && python /c/Users/magic/AppData/Local/Temp/x/run.py --fast')).toBe('python run.py --fast')
  expect(short('C:\\Users\\magic\\foo\\bar.ps1 -Force')).toBe('bar.ps1 -Force')
  expect(short('python "C:\\Users\\magic\\My Docs\\build\\run.py" -v')).toBe('python run.py -v')
  expect(short("node '/c/Users/magic/My Docs/app/server.js'")).toBe('node server.js')
  expect(short('cd "/c/Users/magic/My Docs/proj"; cd sub && npm run watch')).toBe('npm run watch')
  expect(short('cd x; ls -la src/a.py /dev/null')).toBe('ls -la src/a.py /dev/null')
  expect(short('curl https://example.com/a/b/c/d.json')).toBe('curl https://example.com/a/b/c/d.json')
  expect(short('cd /c/Users/magic/AppData/Local/Temp/x')).toBe('Bash')
  expect(shortenCommand('   ', 'Monitor')).toBe('Monitor')
})

test('a background shell without a description shows a short title', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'bg1' } }))
  await start($)
  await $.tool.call({
    tool: 'Bash',
    command: 'cd /c/Users/magic/AppData/Local/Temp/job && python /c/Users/magic/AppData/Local/Temp/job/run.py',
    run_in_background: true,
    tool_use_id: 't60',
  })
  await clock.advance(2_000)
  const tasks = ha.lastReport()?.tasks as Array<{ title: string }>
  expect(tasks.map(t => t.title)).toEqual(['python run.py'])
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
  const cut = progressReport('mcp__savvy-progress__step', { done: 1, total: 2, note: 'x'.repeat(200) })
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
  // What the next model response asks for. A step tool's owner answers the call in its own `tool.call` hook, above
  // ours, so no tool hook of ours and no PostToolUse hears it: the rows the loop keeps are where every plugin reads it.
  let toolUses: Array<{ name: string; input: unknown }> = []
  on('turn.step', async function* () {
    return { turnId: 'turn-x', index: 0, answer: '', toolUses, stopReason: 'tool_use', usage: null } as never
  })
  let callIds = 0
  /** The step tool's answer to call `id` of a loop, as the loop keeps it: a tool-result row, refused (`is_error`) or not. */
  const answerStep = async (agentId: string | undefined, id: string, refused = false) => {
    const result = { type: 'tool_result', tool_use_id: id, content: refused ? 'note is too long' : 'Noted.', ...(refused ? { is_error: true } : {}) }
    await $.session.append({
      message: { type: 'user', role: 'user', content: [result] },
      door: 'tool-result',
      origin: { kind: 'tool', tool: 'mcp__clawd-hq__step' },
      uuid: `row-${id}-result`,
      ...(agentId ? { agentId } : {}),
    })
  }
  /**
   * One model request of a loop (`agentId`; none for the main loop) whose response asks for `uses`, and the rows the
   * engine keeps of it: one per call, then the step tool's answer to each step call (`answer`: taken, refused, or not
   * yet given). Resolves to the ids of the calls.
   */
  const modelStep = async (agentId: string | undefined, uses: typeof toolUses, answer: 'taken' | 'refused' | 'none' = 'taken') => {
    toolUses = uses
    const stream = ($.turn.step as (e: unknown) => AsyncIterable<unknown>)({ turnId: 'turn-x', index: 0, model: 'claude', messageCount: 1, agentId })
    for await (const _ of stream) void _
    const ids = uses.map(() => `toolu-${++callIds}`)
    for (const [n, use] of uses.entries()) {
      await $.session.append({
        message: { type: 'assistant', role: 'assistant', content: [{ type: 'tool_use', id: ids[n], name: use.name, input: use.input }] },
        door: 'response',
        origin: { kind: 'model', model: 'claude' },
        uuid: `row-${ids[n]}`,
        ...(agentId ? { agentId } : {}),
      })
    }
    if (answer !== 'none') for (const [n, use] of uses.entries()) if (use.name.endsWith('__step')) await answerStep(agentId, ids[n]!, answer === 'refused')
    return ids
  }
  await start($)
  await $.tool.call({ tool: 'Agent', description: 'Review', run_in_background: true, tool_use_id: 't11' })
  await clock.advance(2_000)
  const task = () => (ha.lastReport()?.tasks as Array<Record<string, unknown>>)[0]!
  return { clock, ha, task, agents, modelStep, answerStep }
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
    wait: false,
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

test("a background agent's progress comes from its loop's model response, whoever answers the step tool", { options: OPTIONS }, async ($, on) => {
  const { clock, task, modelStep } = await withAgent($, on)
  expect(task().total).toBe(undefined)
  expect(task().calls).toBe(0)

  await modelStep('a1', [
    { name: 'Read', input: { file_path: 'a.md' } },
    { name: 'mcp__clawd-hq__step', input: { done: 2, total: 5, note: 'Writing tests' } },
  ])
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([2, 5, 'Writing tests'])
  // A call the response asks for is no call that ran: the tool.call hook counts those.
  expect(task().calls).toBe(0)

  await modelStep('a1', [{ name: 'Bash', input: { command: 'ls' } }, { name: 'mcp__savvy-progress__step', input: { done: 3, total: 5 } }])
  await clock.advance(2_000)
  expect([task().done, task().total, task().note, task().calls]).toEqual([3, 5, '', 0])

  // Another loop's responses, or the main loop's, are not this task's.
  await modelStep('zz', [{ name: 'Bash', input: {} }, { name: 'mcp__clawd-hq__step', input: { done: 4, total: 5 } }])
  await modelStep(undefined, [{ name: 'Bash', input: {} }, { name: 'mcp__clawd-hq__step', input: { done: 4, total: 5 } }])
  await clock.advance(2_000)
  expect([task().done, task().calls]).toEqual([3, 0])
})

test('a step report its tool refuses never moves the bar', { options: OPTIONS }, async ($, on) => {
  const { clock, task, modelStep } = await withAgent($, on)
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 2, total: 5, note: 'Writing tests' } }])
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([2, 5, 'Writing tests'])

  // A report that looks fine to us, refused all the same (say no hook of the tool's owner answered it).
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 3, total: 5, note: 'Running the suite' } }], 'refused')
  // A note past the tool's 80 characters: the tool refuses it whole, it never cuts it.
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 4, total: 5, note: 'x'.repeat(100) } }], 'refused')
  await clock.advance(2_000)
  expect([task().done, task().total, task().note]).toEqual([2, 5, 'Writing tests'])
})

test('a step report moves the bar once its tool took it, not when the model asks for it', { options: OPTIONS }, async ($, on) => {
  const { clock, task, modelStep, answerStep } = await withAgent($, on)
  const [id] = await modelStep('a1', [{ name: 'mcp__savvy-progress__step', input: { done: 1, total: 4 } }], 'none')
  await clock.advance(2_000)
  expect(task().total).toBe(undefined)

  // Another loop's answer under the same id is not this call's; the answer of the agent's own loop is.
  await answerStep('zz', id!)
  await clock.advance(2_000)
  expect(task().total).toBe(undefined)
  await answerStep('a1', id!)
  await clock.advance(2_000)
  expect([task().done, task().total]).toEqual([1, 4])
})

test("a step report Clawd HQ's step tool refuses is no progress; savvy-progress takes more", () => {
  const clawd = (input: Record<string, unknown>) => progressReport('mcp__clawd-hq__step', input)
  const savvy = (input: Record<string, unknown>) => progressReport('mcp__savvy-progress__step', input)
  expect(clawd({ done: 1, total: 12 })).toEqual({ done: 1, total: 12 })
  expect(clawd({ done: 1, total: 13 })).toBe(null)
  expect(savvy({ done: 1, total: 13 })).toEqual({ done: 1, total: 13 })
  // The note is counted as the tool counts it: on its clean text, in whole characters.
  expect(clawd({ done: 1, total: 2, note: 'x'.repeat(80) })?.note).toBe('x'.repeat(80))
  expect(clawd({ done: 1, total: 2, note: 'x'.repeat(81) })).toBe(null)
  expect(clawd({ done: 1, total: 2, note: ` ${'x'.repeat(80)}​\n` })?.note).toBe('x'.repeat(80))
  expect(clawd({ done: 1, total: 2, note: '\u{1F600}'.repeat(80) })).not.toBe(null)
  // A note that is no text is refused by both (the step tool, or the schema savvy-progress registers).
  expect(clawd({ done: 1, total: 2, note: 5 })).toBe(null)
  expect(savvy({ done: 1, total: 2, note: null })).toBe(null)
  // savvy-progress takes a long note; the screen gets it cut.
  expect(savvy({ done: 1, total: 2, note: 'x'.repeat(200) })?.note?.length).toBe(80)
})

test('a shell has no tool calls to count', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'b1' } }))
  await start($)
  await $.tool.call({ tool: 'Bash', command: 'sleep 99', description: 'Wait', run_in_background: true, tool_use_id: 't1' })
  await clock.advance(2_000)
  const task = (ha.lastReport()?.tasks as Array<Record<string, unknown>>)[0]!
  expect([task.id, task.calls]).toEqual(['b1', undefined])
})

test("a background agent's task ends with its loop's turn, and leaves the main loop's question be", { options: OPTIONS }, async ($, on) => {
  on('turn.complete', () => ({ text: '' }))
  let answer: ((value: ToolCallResult) => void) | null = null
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
  let answer: ((value: ToolCallResult) => void) | null = null
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

/** An agent's notification when it ended its turn with its own background work still running: interim, not its end. */
const interimNotification = (id: string) =>
  [
    '<task-notification>',
    `<task-id>${id}</task-id>`,
    '<tool-use-id>toolu_x</tool-use-id>',
    `<output-file>C:\\tmp\\${id}.output</output-file>`,
    '<status>completed</status>',
    '<summary>Agent "Probe A" is waiting on its own background work</summary>',
    '<note>This agent stopped with background work of its own still running. It may resume on its own when that work completes or reports, and the same task-id notifies again if it does; the result below may be interim.</note>',
    '<result>Started the probe; waiting for it.</result>',
    '</task-notification>',
  ].join('\n')

/** A task's final notification. */
const finalNotification = (id: string) =>
  [
    '<task-notification>',
    `<task-id>${id}</task-id>`,
    '<tool-use-id>toolu_y</tool-use-id>',
    `<output-file>C:\\tmp\\${id}.output</output-file>`,
    '<status>completed</status>',
    `<summary>Task ${id} completed</summary>`,
    '<result>done</result>',
    '</task-notification>',
  ].join('\n')

test('a notification saying the agent waits on its own background work ends nothing; a final one beside it does', () => {
  expect(endedTasks(`${interimNotification('a1')}\n${finalNotification('a2')}`)).toEqual(['a2'])
  // Either signal alone, in any case, makes it interim.
  const one = (id: string, inner: string) => `<task-notification><task-id>${id}</task-id><status>completed</status>${inner}</task-notification>`
  expect(endedTasks(one('a3', '<summary>Agent "X" IS WAITING ON ITS OWN BACKGROUND WORK</summary>'))).toEqual([])
  expect(endedTasks(one('a4', '<note>This agent Stopped with background work of its own still running.</note>'))).toEqual([])
  expect(endedTasks(one('a5', '<note>It May Resume on its own.</note>'))).toEqual([])
  // The words in the result are the agent's own, no note: the notification is final.
  expect(endedTasks(one('a6', '<result>the server may resume later</result>'))).toEqual(['a6'])
  // The real final note of an agent says it may be resumed too; that is not 'resume on its own'.
  const finalNote =
    '<note>A task-notification fires each time this agent stops with no live background children of its own. The user can send it another message and resume it, so the same task-id may notify more than once.</note>'
  expect(endedTasks(one('a7', `<summary>Agent "X" completed</summary>${finalNote}`))).toEqual(['a7'])
})

test("an agent's interim notification keeps it and its shells; its final one ends both", { options: OPTIONS }, async ($, on) => {
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'b7' } }))
  const { clock, ha } = await withAgent($, on)
  await $.tool.call({ tool: 'Bash', command: 'sleep 60', description: 'Probe', run_in_background: true, agentId: 'a1', tool_use_id: 't50' })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1', 'b7'])

  await $.prompt.submit({ text: interimNotification('a1'), origin: { kind: 'task-notification' }, wait: false })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1', 'b7'])

  await $.prompt.submit({ text: finalNotification('a1'), origin: { kind: 'task-notification' }, wait: false })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
})

/**
 * A row as `session.append` raises it: by default a notification delivered into a running turn, an attachment row of
 * type `queued_command` the engine wrote (`origin.kind` engine), on the main loop.
 */
function appendRow(text: string, row: Partial<Omit<SessionAppendInput, 'message'>> & { type?: SessionAppendInput['message']['type'] } = {}): SessionAppendInput {
  const { type = 'attachment', ...rest } = row
  const message: SessionAppendInput['message'] =
    type === 'attachment'
      ? { type, name: 'queued_command', isMeta: true, content: [{ type: 'text', text }] }
      : { type, role: type === 'assistant' ? 'assistant' : 'user', content: [{ type: 'text', text }] }
  return { message, door: 'attachment', origin: { kind: 'engine' }, uuid: `row-${text.length}-${type}`, ...rest }
}

test("a shell's notification delivered into a running main turn ends its task", { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  let n = 0
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: `bg${++n}` } }))
  await start($)
  await $.tool.call({ tool: 'Bash', command: 'npm test', run_in_background: true, tool_use_id: 't60' })
  await $.tool.call({ tool: 'Bash', command: 'npm run build', run_in_background: true, tool_use_id: 't61' })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg1', 'bg2'])

  await $.session.append(appendRow(finalNotification('bg1')))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg2'])

  // The message queue's record of a notification counts too; a monitor's event (no status) ends nothing.
  const event = '<task-notification><task-id>bg2</task-id><event>line 4</event></task-notification>'
  await $.session.append(appendRow(event, { type: 'user', door: 'queue', origin: { kind: 'task-notification' } }))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg2'])
  await $.session.append(appendRow(finalNotification('bg2'), { type: 'user', door: 'queue', origin: { kind: 'task-notification' } }))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
})

test("a subagent's shell whose notification comes into the agent's running turn ends; the agent stays", { options: OPTIONS }, async ($, on) => {
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'b7' } }))
  const { clock, ha } = await withAgent($, on)
  await $.tool.call({ tool: 'Bash', command: 'sleep 60', description: 'Probe', run_in_background: true, agentId: 'a1', tool_use_id: 't62' })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1', 'b7'])

  await $.session.append(appendRow(finalNotification('b7'), { agentId: 'a1' }))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['a1'])
})

test('notification text in a tool result, a tool message, a model response or a typed prompt ends nothing', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  classic(on)
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: 'bg1' } }))
  await start($)
  await $.tool.call({ tool: 'Bash', command: 'npm test', run_in_background: true, tool_use_id: 't63' })

  // Say the model read a transcript file: its result, and what the model says of it, hold the very text.
  const text = finalNotification('bg1')
  await $.session.append(appendRow(text, { type: 'user', door: 'tool-result', origin: { kind: 'tool', tool: 'Read' } }))
  await $.session.append(appendRow(text, { type: 'user', door: 'tool-message', origin: { kind: 'tool', tool: 'Read' } }))
  await $.session.append(appendRow(text, { type: 'assistant', door: 'response', origin: { kind: 'model', model: 'claude' } }))
  await $.session.append(appendRow(text, { type: 'user', door: 'prompt', origin: { kind: 'composer' } }))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['bg1'])

  // The same text as the engine's notification ends it.
  await $.session.append(appendRow(text, { type: 'user', door: 'prompt', origin: { kind: 'task-notification' } }))
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual([])
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
  let answer: ((value: ToolCallResult) => void) | null = null
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

test('a step report is bounded as the integration bounds it', () => {
  // Clawd HQ's step tool takes at most 12 steps; savvy-progress names no bound.
  expect(progressReport('mcp__savvy-progress__step', { done: 20_000, total: 30_000 })).toEqual({ done: 10_000, total: 10_000 })
  expect(progressReport('mcp__savvy-progress__step', { done: 5, total: 1e30 })).toEqual({ done: 5, total: 10_000 })
  expect(progressReport('mcp__savvy-progress__step', { done: 1e30, total: 5 })).toBe(null)
  expect(progressReport('mcp__savvy-progress__step', { done: 1, total: Infinity })).toBe(null)
})

/** A host for a SessionBridge on its own: a clock that says `clock.now`, an agent list, a Home Assistant that answers. */
function bareHost(clock: { now: number }): Host {
  return {
    fetch: async () => ({ status: 200, ok: true, headers: {}, text: '{"ok": true}' }),
    now: async () => clock.now,
    after: () => ({ cancel: () => {} }),
    every: () => ({ cancel: () => {} }),
    log: () => {},
    toast: () => {},
    storeGet: async () => undefined,
    storeSet: async () => {},
    sessionId: async () => 's',
    cwd: async () => '/x',
    model: async () => 'claude',
    usage: async () => ({ context: { window: 1, tokens: 0, percent: 0 }, rateLimits: [] }),
    agents: async () => [],
  } as unknown as Host
}

test("an agent's tool calls stop counting at a million", () => {
  const s = new SessionBridge('i', 's', readConfig(OPTIONS))
  s.startTask({ id: 'a1', kind: 'agent', title: 'x' }, 0)
  s.tasks.set('a1', { ...s.tasks.get('a1')!, calls: 999_999 })
  s.countCall('a1')
  s.countCall('a1')
  expect(s.tasks.get('a1')?.calls).toBe(1_000_000)
})

test('a task past a day is gone from the session at the tick, not only from the report', async () => {
  const clock = { now: 0 }
  const s = new SessionBridge('i', 's', readConfig(OPTIONS))
  s.startTask({ id: 'b1', kind: 'shell', title: 'old' }, 0)
  clock.now = 3_600_000
  s.startTask({ id: 'b2', kind: 'shell', title: 'young' }, clock.now)
  clock.now = 24 * 3_600_000 + 1
  await s.tick(bareHost(clock))
  expect(Array.from(s.tasks.keys())).toEqual(['b2'])
})

test("an agent's background shells and monitors never push it out, and end with it", { options: OPTIONS }, async ($, on) => {
  let n = 0
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false, backgroundTaskId: `s${++n}` } }))
  on('tool.call', { tool: 'Monitor' }, () => ({ result: { taskId: `m${++n}` } }))
  const { clock, ha, modelStep } = await withAgent($, on)
  for (let i = 0; i < 6; i++) await $.tool.call({ tool: 'Bash', command: 'sleep 99', run_in_background: true, agentId: 'a1', tool_use_id: `u${i}` })
  for (let i = 0; i < 3; i++) await $.tool.call({ tool: 'Monitor', command: 'tail -f log', description: 'Watch', agentId: 'a1', tool_use_id: `v${i}` })
  await clock.advance(2_000)
  // Ten tasks for eight places: the oldest shells give way, not the agent.
  expect(taskIds(ha)).toEqual(['a1', 's3', 's4', 's5', 's6', 'm7', 'm8', 'm9'])
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 1, total: 3 } }])
  await clock.advance(2_000)
  const agent = (ha.lastReport()?.tasks as Array<Record<string, unknown>>).find(t => t.id === 'a1')!
  expect([agent.done, agent.total]).toEqual([1, 3])

  // The main loop's own shell is nobody's child; the agent's end takes its shells and monitors with it.
  await $.tool.call({ tool: 'Bash', command: 'npm run watch', run_in_background: true, tool_use_id: 'main1' })
  await $.prompt.submit({
    text: '<task-notification><task-id>a1</task-id><status>completed</status></task-notification>',
    origin: { kind: 'task-notification' },
    wait: false,
  })
  await clock.advance(2_000)
  expect(taskIds(ha)).toEqual(['s10'])
})

test("an agent's calls count once each, and only the ones that ran", { options: OPTIONS }, async ($, on) => {
  on('tool.call', { tool: 'Read' }, () => ({ result: { type: 'text' } }))
  on('tool.call', { tool: 'Write' }, () => ({ deny: 'refused' }))
  const { clock, task, modelStep } = await withAgent($, on)
  // The response asks for a Read and a step: nothing counted yet.
  await modelStep('a1', [
    { name: 'Read', input: { file_path: 'a.md' } },
    { name: 'Write', input: { file_path: 'a.md' } },
    { name: 'mcp__clawd-hq__step', input: { done: 1, total: 2 } },
  ])
  await $.tool.call({ tool: 'Read', file_path: 'a.md', agentId: 'a1', tool_use_id: 'r1' })
  // A refused call, the step tool's own call, another loop's and the main loop's: none of them is the agent's work.
  await $.tool.call({ tool: 'Write', file_path: 'a.md', content: '', agentId: 'a1', tool_use_id: 'r2' })
  await $.tool.call({ tool: 'mcp__clawd-hq__step', done: 2, total: 2, agentId: 'a1', tool_use_id: 'r3' })
  await $.tool.call({ tool: 'Read', file_path: 'a.md', agentId: 'zz', tool_use_id: 'r4' })
  await $.tool.call({ tool: 'Read', file_path: 'a.md', tool_use_id: 'r5' })
  await clock.advance(16_000)
  expect(task().calls).toBe(1)
})

test('a counted call rides on the heartbeat; a step is reported at once', { options: OPTIONS }, async ($, on) => {
  on('tool.call', { tool: 'Read' }, () => ({ result: { type: 'text' } }))
  const { clock, ha, task, modelStep } = await withAgent($, on)
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 1, total: 3 } }])
  await clock.advance(2_000)
  const before = ha.reports().length
  await $.tool.call({ tool: 'Read', file_path: 'a.md', agentId: 'a1', tool_use_id: 'r1' })
  await clock.advance(3_000)
  expect(ha.reports().length).toBe(before)
  await clock.advance(13_000)
  expect(task().calls).toBe(1)
  const later = ha.reports().length
  await modelStep('a1', [{ name: 'mcp__clawd-hq__step', input: { done: 2, total: 3 } }])
  await clock.advance(2_000)
  expect(ha.reports().length).toBe(later + 1)
  expect(task().done).toBe(2)
})

test('a row hook that fails says so once, and the row is kept once', { options: OPTIONS }, async ($, on) => {
  const kept = mock.session(on)
  const { ha } = await withAgent($, on)
  const broken = { type: 'assistant' as const, role: 'assistant' as const, content: [null] as never }
  for (const uuid of ['row-b1', 'row-b2']) await $.session.append({ message: broken, door: 'response', origin: { kind: 'model', model: 'claude' }, uuid, agentId: 'a1' })
  expect(failed(ha, 'session.append').length).toBe(1)
  expect(kept.appended().length).toBe(2)
})

const failed = (ha: ReturnType<typeof mockHa>, where: string) => ha.logs.filter(l => l.startsWith(`tessera-bridge: ${where} failed`))

test('a tool runs once when the bookkeeping after it fails, and the failure is said', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  let runs = 0
  on('tool.call', { tool: 'Bash' }, () => {
    runs += 1
    return { result: { stdout: 'ok', stderr: '', interrupted: false, backgroundTaskId: `b${runs}` } }
  })
  await start($)
  ha.idFails = true
  const ran = (await $.tool.call({ tool: 'Bash', command: 'ls', run_in_background: true, tool_use_id: 't1' })) as { result?: { stdout?: string } }
  await $.tool.call({ tool: 'Bash', command: 'ls', run_in_background: true, tool_use_id: 't2' })
  ha.idFails = false
  await clock.advance(2_000)
  expect(runs).toBe(2)
  expect(ran.result?.stdout).toBe('ok')
  expect(failed(ha, 'tool.call').length).toBe(1)
})

test('a check runs once when the bookkeeping after it fails, and the failure is said', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  let checks = 0
  on('tool.check', () => {
    checks += 1
    return { decision: 'ask', reason: 'mode' }
  })
  await start($)
  ha.idFails = true
  const verdict = await $.tool.check({ tool: 'Bash', input: { command: 'rm -rf build' }, tool_use_id: 't2' })
  ha.idFails = false
  await clock.advance(2_000)
  expect(checks).toBe(1)
  expect(verdict.decision).toBe('ask')
  expect(failed(ha, 'tool.check').length).toBe(1)
})

test('a question opens one dialog when the bookkeeping after it fails, and is not left open', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = mockHa(on)
  let dialogs = 0
  on('tool.call', { tool: 'AskUserQuestion' }, () => {
    dialogs += 1
    return { result: { questions: [], answers: { 'Weiter machen?': 'Ja' } } }
  })
  await start($)
  // A reply the mod cannot read breaks the report of the question, after the dialog was opened.
  ha.questions['toolu_q20'] = null
  const result = (await $.tool.call({ tool: 'AskUserQuestion', questions: TWO, tool_use_id: 'toolu_q20' })) as Answered
  delete ha.questions['toolu_q20']
  await clock.advance(2_000)
  expect(dialogs).toBe(1)
  expect(result.result?.answers).toEqual({ 'Weiter machen?': 'Ja' })
  expect(failed(ha, 'AskUserQuestion').length).toBe(1)
  expect(ha.lastReport()?.state).toBe('idle')
})
