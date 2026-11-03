/** Text limits of the Guition/Waveshare class of screens, in bytes (the firmware clips the rest). */
export const LIMITS = { title: 64, subtitle: 240, button: 16 } as const

const encoder = new TextEncoder()
const decoder = new TextDecoder()

/** Cuts `text` to at most `max` UTF-8 bytes on a character boundary, ending a cut text with an ellipsis. */
export function cutBytes(text: string, max: number): string {
  const bytes = encoder.encode(text)
  if (bytes.length <= max) return text
  const ellipsis = encoder.encode('…')
  let end = Math.max(0, max - ellipsis.length)
  // Step back to a UTF-8 boundary: a continuation byte is 10xxxxxx.
  while (end > 0 && ((bytes[end] ?? 0) & 0xc0) === 0x80) end -= 1
  return decoder.decode(bytes.subarray(0, end)) + '…'
}

export type AlertSpec = {
  title: string
  subtitle: string
  buttonText: string
  /** Written to the answer entity when the right button is pressed. */
  buttonValue?: string
  button2Text?: string
  /** Written to the answer entity when the left button is pressed. */
  button2Value?: string
  icon?: string
  color?: string
  buttonColor?: string
  button2Color?: string
  /** Seconds until the card goes away on its own; 0 waits for a button. */
  timeout?: number
  flash?: boolean
}

export type AlertPayload = Record<string, string | number | boolean | Record<string, string>>

/**
 * Builds the data of an `esp_screens_show_alert` event: a card over the whole
 * screen with one button, or two for a choice, each writing its value to the
 * answer entity through `input_text.set_value`.
 */
export function buildAlert(spec: AlertSpec, target: { screen: string; answerEntity: string }): AlertPayload {
  const payload: AlertPayload = {
    title: cutBytes(spec.title, LIMITS.title),
    subtitle: cutBytes(spec.subtitle, LIMITS.subtitle),
    icon: spec.icon ?? 'robot',
    color: spec.color ?? 'blue',
    button_text: cutBytes(spec.buttonText, LIMITS.button),
    button_color: spec.buttonColor ?? 'blue',
    timeout: spec.timeout ?? 0,
    flash: spec.flash ?? true,
  }
  if (target.screen) payload.screen = target.screen
  if (spec.buttonValue !== undefined) {
    payload.action = 'input_text.set_value'
    payload.data = { entity_id: target.answerEntity, value: spec.buttonValue }
  }
  if (spec.button2Text) {
    payload.button2_text = cutBytes(spec.button2Text, LIMITS.button)
    payload.button2_color = spec.button2Color ?? 'gray'
    if (spec.button2Value !== undefined) {
      payload.button2_action = 'input_text.set_value'
      payload.button2_data = { entity_id: target.answerEntity, value: spec.button2Value }
    }
  }
  return payload
}

/** One line that says what a tool call is about, for the permission hint. */
export function describeCall(tool: string, input: unknown): string {
  const i = (input ?? {}) as Record<string, unknown>
  const pick = (key: string) => (typeof i[key] === 'string' ? (i[key] as string) : undefined)
  const detail =
    pick('command') ?? pick('description') ?? pick('file_path') ?? pick('path') ?? pick('url') ?? pick('pattern')
  if (detail) return `${tool}: ${detail.replace(/\s+/g, ' ')}`
  const json = JSON.stringify(i)
  return json && json !== '{}' ? `${tool}: ${json}` : tool
}
