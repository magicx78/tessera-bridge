import { expect, mock, test } from 'claude-code/testing'
import type { TestBody } from 'claude-code/testing'

import { buildAlert, cutBytes, describeCall } from './alert'
import { formatRemaining } from './register'

const OPTIONS = {
  haUrl: 'http://ha.test:8123',
  haToken: 'test-token',
  screen: '',
  answerEntity: 'input_text.claude_screen_answer',
  permissionHints: 'on',
  hintDelay: '5',
}

type Call = { method: string; url: string; body: unknown }

/** Answers `$.http.fetch` from memory and records every call; `answer` is what the answer entity holds. */
function mockHa(on: Parameters<TestBody>[1], state: { answer: string }) {
  const calls: Call[] = []
  on('http.fetch', (_, e) => {
    const init = e.init ?? {}
    calls.push({ method: init.method ?? 'GET', url: e.url, body: init.body ? JSON.parse(init.body) : undefined })
    if (e.url.endsWith('/api/states/input_text.claude_screen_answer') && (init.method ?? 'GET') === 'GET') {
      return { value: { status: 200, ok: true, headers: {}, text: JSON.stringify({ state: state.answer }) } }
    }
    return { value: { status: 200, ok: true, headers: {}, text: '{}' } }
  })
  return calls
}

test('cutBytes keeps a UTF-8 boundary and adds an ellipsis', async () => {
  expect(cutBytes('Hallo', 64)).toBe('Hallo')
  const cut = cutBytes('Schöne Grüße aus Oerlinghausen', 16)
  expect(new TextEncoder().encode(cut).length <= 16).toBe(true)
  expect(cut.endsWith('…')).toBe(true)
})

test('buildAlert wires both buttons to the answer entity', async () => {
  const payload = buildAlert(
    { title: 'T', subtitle: 'S', buttonText: 'Ja', buttonValue: 'n|0', button2Text: 'Nein', button2Value: 'n|1' },
    { screen: 'waveshare-p4-2', answerEntity: 'input_text.x' },
  )
  expect(payload.screen).toBe('waveshare-p4-2')
  expect(payload.action).toBe('input_text.set_value')
  expect(payload.data).toEqual({ entity_id: 'input_text.x', value: 'n|0' })
  expect(payload.button2_action).toBe('input_text.set_value')
  expect(payload.button2_data).toEqual({ entity_id: 'input_text.x', value: 'n|1' })
  expect(payload.timeout).toBe(0)
})

test('describeCall and formatRemaining read well', async () => {
  expect(describeCall('Bash', { command: 'git   status' })).toBe('Bash: git status')
  expect(describeCall('Read', { file_path: '/tmp/a' })).toBe('Read: /tmp/a')
  expect(
    describeCall('mcp__237d5e7f-01a6-419f-b176-a9833f3d3a3c__ha_call_service', {
      domain: 'light',
      service: 'turn_on',
      entity_id: 'light.kitchen',
    }),
  ).toBe('ha_call_service: light.turn_on → light.kitchen')
  expect(describeCall('mcp__ha-tools__ha_search', { query: 'lamp' })).toBe('ha_search (ha-tools): lamp')
  expect(formatRemaining('2026-01-01T01:09:00Z', Date.parse('2026-01-01T00:00:00Z'))).toBe('1h 09m')
  expect(formatRemaining(undefined, 0)).toBe('–')
})

test('a question answered on the screen is answered in the app', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const ha = { answer: '' }
  const calls = mockHa(on, ha)
  let dialogShown = false
  on('tool.call', { tool: 'AskUserQuestion' }, () => {
    dialogShown = true
    // The dialog in the app never answers in this test: the screen does.
    return new Promise(() => {})
  })

  const questions = [
    { question: 'Weiter machen?', header: 'Weiter', multiSelect: false, options: [{ label: 'Ja', description: '' }, { label: 'Nein', description: '' }] },
  ]
  const call = $.tool.call({ tool: 'AskUserQuestion', questions })

  await clock.advance(10)
  const alert = calls.find(c => c.url.endsWith('/api/events/esp_screens_show_alert'))
  expect(alert).toBeDefined()
  const body = alert!.body as Record<string, unknown>
  expect(body.button_text).toBe('Ja')
  expect(body.button2_text).toBe('Nein')
  const nonce = String((body.data as { value: string }).value).split('|')[0]

  // Minutes pass while nobody answers: the poll keeps running, nothing breaks.
  await clock.advance(120_000)
  ha.answer = `${nonce}|1`
  await clock.advance(2_000)

  const result = await call
  expect(dialogShown).toBe(true)
  expect((result as { result?: { answers?: Record<string, string> } }).result?.answers).toEqual({
    'Weiter machen?': 'Nein',
  })
  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_dismiss_alert'))).toBe(true)
})

test('an answer in the app takes the alert off the screen', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const calls = mockHa(on, { answer: '' })
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({
    result: { questions: [], answers: { 'Weiter machen?': 'Ja' } },
  }))

  const result = await $.tool.call({
    tool: 'AskUserQuestion',
    questions: [{ question: 'Weiter machen?', header: 'Weiter', multiSelect: false, options: [{ label: 'Ja', description: '' }, { label: 'Nein', description: '' }] }],
  })
  await clock.advance(10)

  expect((result as { result?: { answers?: Record<string, string> } }).result?.answers).toEqual({ 'Weiter machen?': 'Ja' })
  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_dismiss_alert'))).toBe(true)
})

test('a Home Assistant that is down leaves the dialog alone', { options: OPTIONS }, async ($, on) => {
  mock.clock(on)
  on('http.fetch', () => ({ value: { status: 503, ok: false, headers: {}, text: '' } }))
  on('tool.call', { tool: 'AskUserQuestion' }, () => ({ result: { questions: [], answers: { Q: 'A' } } }))

  const result = await $.tool.call({
    tool: 'AskUserQuestion',
    questions: [{ question: 'Q', header: 'H', multiSelect: false, options: [{ label: 'A', description: '' }, { label: 'B', description: '' }] }],
  })
  expect((result as { result?: { answers?: Record<string, string> } }).result?.answers).toEqual({ Q: 'A' })
})

test('a measurement pushes the limits as sensors', { options: OPTIONS }, async ($, on) => {
  mock.clock(on, { now: Date.parse('2026-01-01T00:00:00Z') })
  const calls = mockHa(on, { answer: '' })
  on('session.measure', (_, e) => ({ changed: e.changed, cwd: '/' }))

  await $.session.measure({
    context: { window: 1_000_000, tokens: 110_000, percent: 11 },
    rateLimits: [
      { kind: 'five_hour', percentUsed: 44, resetsAt: '2026-01-01T01:09:00Z' },
      { kind: 'seven_day', percentUsed: 11.5, resetsAt: '2026-01-07T00:00:00Z' },
    ],
    changed: ['rateLimits', 'context'],
  })

  const posted = Object.fromEntries(
    calls.filter(c => c.url.includes('/api/states/')).map(c => [c.url.split('/api/states/')[1], c.body as { state: string; attributes: Record<string, unknown> }]),
  )
  expect(posted['sensor.claude_limit_5h']?.state).toBe('44')
  expect(posted['sensor.claude_limit_5h']?.attributes.resets_in).toBe('1h 09m')
  expect(posted['sensor.claude_limit_week']?.state).toBe('12')
  expect(posted['sensor.claude_5h_reset']?.state).toBe('1h 09m')
  expect(posted['sensor.claude_context']?.state).toBe('11')
})

test('a permission ask that stays open shows a hint, and the turn end takes it down', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const calls = mockHa(on, { answer: '' })
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))
  on('turn.complete', () => ({ text: '' }))

  const verdict = await $.tool.check({ tool: 'Bash', input: { command: 'rm -rf build' }, tool_use_id: 't1', approver: 'person' })
  expect(verdict.decision).toBe('ask')
  // Nothing yet: the ask may settle by itself.
  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_show_alert'))).toBe(false)

  await clock.advance(6_000)
  const hint = calls.find(c => c.url.endsWith('/api/events/esp_screens_show_alert'))
  expect(hint).toBeDefined()
  expect(String((hint!.body as { subtitle: string }).subtitle).startsWith('Bash: rm -rf build')).toBe(true)

  await $.turn.complete({ answer: '', durationMs: 1, isAborted: false, turnId: 'turn-1', reason: 'answer' })
  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_dismiss_alert'))).toBe(true)
})

test('an ask that settles quickly shows nothing on the screen', { options: OPTIONS }, async ($, on) => {
  const clock = mock.clock(on)
  const calls = mockHa(on, { answer: '' })
  on('tool.check', () => ({ decision: 'ask', reason: 'mode' }))
  on('tool.call', { tool: 'Bash' }, () => ({ result: { stdout: '', stderr: '', interrupted: false } }))

  await $.tool.check({ tool: 'Bash', input: { command: 'ls' }, tool_use_id: 't2', approver: 'person' })
  await $.tool.call({ tool: 'Bash', command: 'ls', tool_use_id: 't2' })
  await clock.advance(10_000)

  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_show_alert'))).toBe(false)
  expect(calls.some(c => c.url.endsWith('/api/events/esp_screens_dismiss_alert'))).toBe(false)
})
