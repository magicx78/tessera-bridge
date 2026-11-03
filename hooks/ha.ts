import type { PluginOptions } from 'claude-code'

import type { Host } from './host'

/** What the bridge needs to know, read from the mod's options (plugin.json, userConfig). */
export type HaConfig = {
  /** Home Assistant's base URL without a trailing slash. */
  url: string
  /** A long-lived access token; empty turns the bridge off. */
  token: string
  /** A name for this computer in the session list, "" for the host name the integration sees. */
  instanceName: string
  /** A name for this session in the session list, "" for the project folder's name. */
  sessionName: string
  permissionHints: boolean
  /** How long a permission ask stays open before it is reported as "waiting for approval", in milliseconds. */
  hintDelayMs: number
}

export function readConfig(options: PluginOptions): HaConfig {
  const str = (key: string, fallback: string) => {
    const v = options[key]
    return typeof v === 'string' ? v.trim() : fallback
  }
  return {
    url: str('haUrl', 'http://homeassistant.local:8123').replace(/\/+$/, ''),
    token: str('haToken', ''),
    instanceName: str('instanceName', ''),
    sessionName: str('sessionName', ''),
    permissionHints: str('permissionHints', 'on') !== 'off',
    hintDelayMs: Math.max(0, Math.min(120, Number(str('hintDelay', '8')) || 8)) * 1000,
  }
}

export type Json = Record<string, unknown>

/** The integration's HTTP API (custom_components/tessera_claude/api.py), under Home Assistant's own /api/. */
export const API_BASE = '/api/tessera_claude/v1'

export class HaError extends Error {
  constructor(
    message: string,
    public readonly status: number,
  ) {
    super(message)
  }
}

/**
 * One JSON request to the integration's API. Resolves the parsed body; rejects with an HaError on a non-2xx status
 * (404 when the integration is not installed) and with the fetch's own error when Home Assistant is unreachable.
 */
export async function apiCall(host: Host, cfg: HaConfig, path: string, body: Json): Promise<Json> {
  const res = await host.fetch(cfg.url + API_BASE + path, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${cfg.token}`,
      'Content-Type': 'application/json',
    },
    body: JSON.stringify(body),
  })
  if (!res.ok) throw new HaError(`HA POST ${API_BASE}${path} answered ${res.status}`, res.status)
  if (!res.text) return {}
  const parsed: unknown = JSON.parse(res.text)
  return parsed !== null && typeof parsed === 'object' ? (parsed as Json) : {}
}

let hasWarned = false

/**
 * Runs `fn`; on failure logs it, shows one toast per module load, and resolves undefined, so a Home Assistant that is
 * down never breaks a tool call or a turn.
 */
export async function quietly<T>(host: Host, fn: () => Promise<T>): Promise<T | undefined> {
  try {
    return await fn()
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    host.log(`tessera-bridge: ${message}`)
    if (!hasWarned) {
      hasWarned = true
      host.toast(`Tessera-Bridge: Home Assistant nicht erreichbar (${message})`)
    }
    return undefined
  }
}
