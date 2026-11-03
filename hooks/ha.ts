import type { EngineInterface, PluginOptions } from 'claude-code'

/** What the bridge needs to know about Home Assistant, read from the options. */
export type HaConfig = {
  url: string
  token: string
  screen: string
  answerEntity: string
  permissionHints: boolean
  /** How long an ask stays open before the screen shows a hint, in milliseconds. */
  hintDelayMs: number
  /** A media_player that plays a chime and the spoken text with each alert; empty for none. */
  soundPlayer: string
  soundChime: string
  soundSpeak: boolean
  soundTts: string
  soundLanguage: string
}

export function readConfig(options: PluginOptions): HaConfig {
  const str = (key: string, fallback: string) => {
    const v = options[key]
    return typeof v === 'string' ? v.trim() : fallback
  }
  return {
    url: str('haUrl', 'http://homeassistant.local:8123').replace(/\/+$/, ''),
    token: str('haToken', ''),
    screen: str('screen', ''),
    answerEntity: str('answerEntity', 'input_text.claude_screen_answer'),
    permissionHints: str('permissionHints', 'on') !== 'off',
    hintDelayMs: Math.max(0, Math.min(120, Number(str('hintDelay', '8')) || 8)) * 1000,
    soundPlayer: str('soundPlayer', ''),
    soundChime: str('soundChime', 'chirp'),
    soundSpeak: str('soundSpeak', 'on') !== 'off',
    soundTts: str('soundTts', ''),
    soundLanguage: str('soundLanguage', 'de-DE'),
  }
}

/**
 * Plays the chime and speaks `text` on the configured player through
 * `chime_tts.say`; nothing when no player is set. Errors stay with the caller.
 */
export async function playSound($: EngineInterface, cfg: HaConfig, text: string): Promise<void> {
  if (!cfg.soundPlayer) return
  const data: Json = { entity_id: cfg.soundPlayer, announce: true }
  if (cfg.soundChime) data.chime_path = cfg.soundChime
  if (cfg.soundSpeak && text) {
    data.message = text
    if (cfg.soundLanguage) data.language = cfg.soundLanguage
    if (cfg.soundTts) data.tts_platform = cfg.soundTts
  }
  if (data.chime_path === undefined && data.message === undefined) return
  await callService($, cfg, 'chime_tts', 'say', data)
}

type Json = Record<string, unknown>

async function request(
  $: EngineInterface,
  cfg: HaConfig,
  method: 'GET' | 'POST',
  path: string,
  body?: Json,
): Promise<string> {
  const res = await $.http.fetch(cfg.url + path, {
    method,
    headers: {
      Authorization: `Bearer ${cfg.token}`,
      'Content-Type': 'application/json',
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
  if (!res.ok) {
    throw new Error(`HA ${method} ${path} answered ${res.status}`)
  }
  return res.text
}

/** Fires an event on the HA event bus (what Tessera listens to). */
export function fireEvent($: EngineInterface, cfg: HaConfig, type: string, data: Json): Promise<string> {
  return request($, cfg, 'POST', `/api/events/${type}`, data)
}

/** Sets an entity's state and attributes through the REST API (creates it when missing). */
export function setState(
  $: EngineInterface,
  cfg: HaConfig,
  entity: string,
  state: string | number,
  attributes: Json,
): Promise<string> {
  return request($, cfg, 'POST', `/api/states/${entity}`, { state: String(state), attributes })
}

/** Reads an entity's current state. */
export async function getState($: EngineInterface, cfg: HaConfig, entity: string): Promise<{ state: string }> {
  const text = await request($, cfg, 'GET', `/api/states/${entity}`)
  const parsed = JSON.parse(text) as { state?: unknown }
  return { state: typeof parsed.state === 'string' ? parsed.state : '' }
}

/** Calls a HA action (service). */
export function callService(
  $: EngineInterface,
  cfg: HaConfig,
  domain: string,
  service: string,
  data: Json,
): Promise<string> {
  return request($, cfg, 'POST', `/api/services/${domain}/${service}`, data)
}

let hasWarned = false

/**
 * Runs `fn`; on failure shows one toast per module load and resolves undefined,
 * so a Home Assistant that is down never breaks a tool call or a turn.
 */
export async function quietly<T>($: EngineInterface, fn: () => Promise<T>): Promise<T | undefined> {
  try {
    return await fn()
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    $.ui.log(`tessera-bridge: ${message}`, { to: 'debug' })
    if (!hasWarned) {
      hasWarned = true
      $.ui.toast(`Tessera-Bridge: Home Assistant nicht erreichbar (${message})`)
    }
    return undefined
  }
}
