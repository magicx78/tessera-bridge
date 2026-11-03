import type { EngineInterface, Register, SessionRateLimit } from 'claude-code'

import { LIMITS, buildAlert, cutBytes, describeCall } from './alert'
import { callService, fireEvent, getState, playSound, quietly, readConfig, setState, type HaConfig } from './ha'

type Question = {
  question: string
  header: string
  kind?: 'choice' | 'text' | 'number'
  options?: Array<{ label: string; description?: string }>
  multiSelect: boolean
}

type ScreenAnswers = Record<string, string>

type ContextUsage = { percent?: number; window: number; tokens?: number }

const POLL_MS = 1500
const MAX_POLLS = 1200 // 30 minutes, then the screen gives up and the PC answers

const NEXT = 'next'
const PC = 'pc'

const KNOWN_LIMITS: Record<string, { id: string; name: string }> = {
  five_hour: { id: 'sensor.claude_limit_5h', name: 'Claude 5h' },
  seven_day: { id: 'sensor.claude_limit_week', name: 'Claude Woche' },
}

/** How long a permission ask has to stay open before the screen shows a hint: in auto mode most asks settle by themselves within a second or two. */
const HINT_DELAY_MS = 5000

/** Module state: lost on a reload, which is fine for these. */
const state = {
  /** The tool_use_id the hint on the screen is about, if one is up. */
  hintFor: null as string | null,
  /** Tool calls whose permission ask is still open. */
  pendingAsks: new Set<string>(),
  lastLimits: [] as SessionRateLimit[],
}

// ── Helpers ───────────────────────────────────────────────────────────────────

/** The screens an alert goes to: one by name, or every paired one. */
function target(cfg: HaConfig): Record<string, string> {
  return cfg.screen ? { screen: cfg.screen } : {}
}

function limitEntity(kind: string): { id: string; name: string } {
  const known = KNOWN_LIMITS[kind]
  if (known) return known
  const slug = kind
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '_')
    .replace(/^_|_$/g, '')
  const rest = kind.replace(/^seven_day_?/, '').replace(/_/g, ' ').trim()
  return {
    id: `sensor.claude_limit_${slug.replace(/^seven_day_/, 'week_')}`,
    name: kind.startsWith('seven_day') ? `Claude Woche ${rest}`.trim() : `Claude ${rest || kind}`,
  }
}

export function formatRemaining(resetsAt: string | undefined, now: number): string {
  if (!resetsAt) return '–'
  const ms = Date.parse(resetsAt) - now
  if (!Number.isFinite(ms) || ms <= 0) return '0m'
  const minutes = Math.floor(ms / 60_000)
  const days = Math.floor(minutes / 1440)
  const hours = Math.floor((minutes % 1440) / 60)
  const mins = minutes % 60
  if (days > 0) return `${days}d ${hours}h`
  if (hours > 0) return `${hours}h ${String(mins).padStart(2, '0')}m`
  return `${mins}m`
}

function dismiss($: EngineInterface, cfg: HaConfig) {
  return quietly($, () => fireEvent($, cfg, 'esp_screens_dismiss_alert', target(cfg)))
}

/** Takes the hint down once the call it was about has finished (or every call, at a turn's end). */
async function settleAsk($: EngineInterface, cfg: HaConfig, toolUseId: string | undefined) {
  if (toolUseId === undefined) state.pendingAsks.clear()
  else state.pendingAsks.delete(toolUseId)
  if (state.hintFor === null) return
  if (toolUseId !== undefined && state.hintFor !== toolUseId) return
  state.hintFor = null
  await dismiss($, cfg)
}

/** Shows the hint for an ask that is still open after HINT_DELAY_MS. */
async function showHintIfStillOpen($: EngineInterface, cfg: HaConfig, toolUseId: string, tool: string, input: unknown) {
  if (!state.pendingAsks.has(toolUseId) || state.hintFor !== null) return
  state.hintFor = toolUseId
  await showPermissionHint($, cfg, tool, input)
}

// ── Usage → Home Assistant sensors ────────────────────────────────────────────

async function writeLimits($: EngineInterface, cfg: HaConfig, now: number) {
  for (const limit of state.lastLimits) {
    const entity = limitEntity(limit.kind)
    await setState($, cfg, entity.id, Math.round(limit.percentUsed), {
      friendly_name: entity.name,
      unit_of_measurement: '%',
      icon: 'mdi:gauge',
      kind: limit.kind,
      resets_at: limit.resetsAt ?? '',
      resets_in: formatRemaining(limit.resetsAt, now),
    })
  }
  const fiveHour = state.lastLimits.find(l => l.kind === 'five_hour')
  await setState($, cfg, 'sensor.claude_5h_reset', formatRemaining(fiveHour?.resetsAt, now), {
    friendly_name: 'Claude Reset',
    icon: 'mdi:timer-outline',
    resets_at: fiveHour?.resetsAt ?? '',
  })
}

async function pushLimits($: EngineInterface, cfg: HaConfig, limits: SessionRateLimit[]) {
  state.lastLimits = limits
  await writeLimits($, cfg, await $.clock.now())
}

function pushContext($: EngineInterface, cfg: HaConfig, context: ContextUsage) {
  return setState($, cfg, 'sensor.claude_context', context.percent ?? 0, {
    friendly_name: 'Claude Kontext',
    unit_of_measurement: '%',
    icon: 'mdi:database',
    tokens: context.tokens ?? 0,
    window: context.window,
  })
}

async function pushUsageNow($: EngineInterface, cfg: HaConfig) {
  const usage = await $.session.usage()
  await pushLimits($, cfg, usage.rateLimits)
  await pushContext($, cfg, usage.context)
}

/** The minute tick: keeps the "resets in" texts moving between measurements. */
async function refreshRemaining($: EngineInterface, cfg: HaConfig) {
  if (state.lastLimits.length === 0) return
  await writeLimits($, cfg, await $.clock.now())
}

// ── Questions → alert on the screen ───────────────────────────────────────────

function clearAnswer($: EngineInterface, cfg: HaConfig) {
  return callService($, cfg, 'input_text', 'set_value', { entity_id: cfg.answerEntity, value: '' })
}

/**
 * Polls the answer entity every POLL_MS, in dispatches of its own, until an
 * answer to this alert lands or `isDone()`. Two writers are accepted: the
 * alert's own button action (`<nonce>|<choice>`) and the HA automation on
 * `esphome.screen_alert` (`ok|<title>` for the right button, `button2|<title>`
 * for the left one). Resolves the choice, or null.
 */
function waitForAnswer(
  $: EngineInterface,
  cfg: HaConfig,
  keys: { nonce: string; title: string; left: string },
  isDone: () => boolean,
) {
  return new Promise<string | null>(resolve => {
    let polls = 0
    const poll = async () => {
      if (isDone() || polls >= MAX_POLLS) return resolve(null)
      polls += 1
      const current = await quietly($, () => getState($, cfg, cfg.answerEntity))
      const value = current?.state ?? ''
      if (value.startsWith(keys.nonce + '|')) return resolve(value.slice(keys.nonce.length + 1))
      if (value === `ok|${keys.title}`) return resolve('0')
      if (value === `button2|${keys.title}`) return resolve(keys.left)
      $.clock.after(POLL_MS, poll)
    }
    void poll()
  })
}

function canAnswerOnScreen(q: Question): boolean {
  return (q.kind === undefined || q.kind === 'choice') && !q.multiSelect && (q.options?.length ?? 0) >= 2
}

async function hintOnScreen($: EngineInterface, cfg: HaConfig, title: string, subtitle: string, timeout: number) {
  await fireEvent(
    $,
    cfg,
    'esp_screens_show_alert',
    buildAlert(
      { title, subtitle, buttonText: 'OK', icon: 'robot', color: 'blue', timeout },
      { screen: cfg.screen, answerEntity: cfg.answerEntity },
    ),
  )
  await quietly($, () => playSound($, cfg, subtitle))
}

/**
 * Shows one question on the screen: one alert for two options, or a chain of
 * alerts (option k on the right, "Weiter ›" on the left) for three or four.
 * Resolves the chosen label, or null when the answer should come from the PC.
 */
async function askOne($: EngineInterface, cfg: HaConfig, q: Question, isDone: () => boolean): Promise<string | null> {
  const options = q.options ?? []
  const nonce = Math.random().toString(36).slice(2, 8)
  const isPair = options.length === 2

  for (let k = 0; k < options.length; k += 1) {
    if (isDone()) return null
    const isLast = k === options.length - 1
    const right = options[k]
    if (!right) return null
    const left = isPair ? options[1] : undefined

    // The right button is option k; what the left one means depends on the page.
    const leftChoice = left ? '1' : isLast ? PC : NEXT
    const title = cutBytes(q.header || 'Claude fragt', LIMITS.title)

    await clearAnswer($, cfg)
    await fireEvent(
      $,
      cfg,
      'esp_screens_show_alert',
      buildAlert(
        {
          title,
          subtitle: isPair ? q.question : `${q.question}\n(${k + 1}/${options.length}) ${right.label}`,
          buttonText: right.label,
          buttonValue: `${nonce}|${k}`,
          button2Text: left ? left.label : isLast ? 'Am PC' : 'Weiter ›',
          button2Value: `${nonce}|${leftChoice}`,
          button2Color: left ? 'gray' : isLast ? 'red' : 'gray',
        },
        { screen: cfg.screen, answerEntity: cfg.answerEntity },
      ),
    )

    // The chime and the spoken question once per question, not per page of options.
    if (k === 0) await quietly($, () => playSound($, cfg, q.question))

    const raw = await waitForAnswer($, cfg, { nonce, title, left: leftChoice }, isDone)
    // '0' from the automation means "the right button", which is option k on this page.
    const choice = raw === '0' ? String(k) : raw
    if (choice === null || choice === PC) return null
    if (choice === NEXT) continue
    const index = Number(choice)
    if (Number.isInteger(index) && options[index]) return options[index].label
    return null
  }
  return null
}

/** Asks every question on the screen in turn; null when any of them goes to the PC. */
async function askOnScreen(
  $: EngineInterface,
  cfg: HaConfig,
  questions: Question[],
  isDone: () => boolean,
): Promise<ScreenAnswers | null> {
  const answers: ScreenAnswers = {}
  for (const q of questions) {
    if (isDone()) return null
    if (!canAnswerOnScreen(q)) {
      await quietly($, () => hintOnScreen($, cfg, q.header || 'Claude fragt', `${q.question}\nAntwort bitte am PC.`, 300))
      return null
    }
    const label = await askOne($, cfg, q, isDone)
    if (label === null) return null
    answers[q.question] = label
  }
  return answers
}

async function showPermissionHint($: EngineInterface, cfg: HaConfig, tool: string, input: unknown) {
  await fireEvent(
    $,
    cfg,
    'esp_screens_show_alert',
    buildAlert(
      {
        title: 'Claude wartet auf Freigabe',
        subtitle: `${describeCall(tool, input)}\nAntwort am PC.`,
        buttonText: 'OK',
        icon: 'lock-alert',
        color: 'orange',
        buttonColor: 'orange',
        timeout: 600,
      },
      { screen: cfg.screen, answerEntity: cfg.answerEntity },
    ),
  )
  await quietly($, () => playSound($, cfg, 'Claude wartet auf eine Freigabe am PC.'))
}

// ── Registration ──────────────────────────────────────────────────────────────

export const register: Register = (on, options) => {
  const cfg = readConfig(options)
  const isOn = () => cfg.token.length > 0

  on('session.start', async ($, e, next) => {
    const started = await next(e)
    if (isOn()) {
      await quietly($, () => pushUsageNow($, cfg))
      $.clock.every(60_000, () => quietly($, () => refreshRemaining($, cfg)))
    }
    return started
  })

  on('session.measure', async ($, e, next) => {
    if (isOn()) {
      if (e.changed.includes('rateLimits')) await quietly($, () => pushLimits($, cfg, e.rateLimits))
      if (e.changed.includes('context')) await quietly($, () => pushContext($, cfg, e.context))
    }
    return next(e)
  })

  on('tool.call', { tool: 'AskUserQuestion' }, async ($, e, next) => {
    if (!isOn()) return next(e)
    const questions = (e.questions ?? []) as Question[]
    if (questions.length === 0) return next(e)

    // The dialog in the app runs beneath, in parallel with the screen.
    let desktopDone = false
    const desktop = next(e)
    desktop.then(
      () => {
        desktopDone = true
      },
      () => {
        desktopDone = true
      },
    )

    const onScreen = askOnScreen($, cfg, questions, () => desktopDone).catch(() => null)

    const first = await Promise.race([
      desktop.then(result => ({ from: 'desktop' as const, result })),
      onScreen.then(answers => ({ from: 'screen' as const, answers })),
    ])

    if (first.from === 'desktop') {
      await dismiss($, cfg)
      return first.result
    }
    if (first.answers === null) {
      // The screen handed the question to the PC: the dialog there stands.
      const result = await desktop
      await dismiss($, cfg)
      return result
    }
    await dismiss($, cfg)
    $.ui.toast('Antwort vom Tessera-Screen übernommen')
    return { result: { questions: e.questions, answers: first.answers } }
  }).catch(($, e, next) => next(e))

  on('tool.check', async ($, e, next) => {
    const verdict = await next(e)
    const id = e.tool_use_id
    if (
      isOn() &&
      cfg.permissionHints &&
      verdict.decision === 'ask' &&
      e.approver === 'person' &&
      id !== undefined &&
      e.tool !== 'AskUserQuestion'
    ) {
      // Not at once: in auto mode most asks settle by themselves in a moment.
      state.pendingAsks.add(id)
      const tool = e.tool
      const input = e.input
      $.clock.after(HINT_DELAY_MS, () => quietly($, () => showHintIfStillOpen($, cfg, id, tool, input)))
    }
    return verdict
  }).catch(($, e, next) => next(e))

  // Once a tool ran (or was refused) the person has answered on the PC: take the hint down.
  on('tool.call', async ($, e, next) => {
    const ran = await next(e)
    await settleAsk($, cfg, e.tool_use_id)
    return ran
  })

  on('turn.complete', async ($, e, next) => {
    await settleAsk($, cfg, undefined)
    return next(e)
  })
}

// Reloaded with options from settings.json (pluginConfigs["tessera-bridge"]).
