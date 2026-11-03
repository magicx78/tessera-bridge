/**
 * The questions of one AskUserQuestion call, as the bridge reports them to Home Assistant and as the display answers
 * them. Identity is never a text: the call's tool_use_id names the question, `p<i>` a part (one of up to four
 * questions of the call), `o<i>` an option. Labels and texts are for showing only.
 */

/** One question of the tool's input, as the engine passes it (sdk-tools.d.ts, AskUserQuestionInput). */
export type RawQuestion = {
  question: string
  header: string
  multiSelect?: boolean
  options?: Array<{ label: string; description?: string }>
}

export type ReportedOption = { id: string; label: string; description: string }

export type ReportedPart = {
  /** `p0` to `p3`. */
  id: string
  header: string
  text: string
  multi: boolean
  options: ReportedOption[]
}

export type ReportedQuestion = {
  /** The call's tool_use_id, or a random id when the engine gave none. */
  id: string
  /** Fresh for every registration, so an answer from before a reload never fits a question registered after it. */
  nonce: string
  parts: ReportedPart[]
}

/** What the integration hands back as the display's answer: option ids per part id. */
export type DisplayAnswer = Record<string, string[]>

/** What the tool call resolves with: the answers keyed by the question's text, labels comma-joined for a multi-select. */
export type ToolAnswers = Record<string, string>

export function nonce(): string {
  return Math.random().toString(36).slice(2, 10)
}

export function toReported(id: string, raw: unknown[]): ReportedQuestion | null {
  const parts: ReportedPart[] = []
  raw.forEach((item, index) => {
    if (item === null || typeof item !== 'object') return
    const q = item as RawQuestion
    if (typeof q.question !== 'string') return
    const options = Array.isArray(q.options) ? q.options : []
    parts.push({
      id: `p${index}`,
      header: typeof q.header === 'string' ? q.header : '',
      text: q.question,
      multi: q.multiSelect === true,
      options: options
        .filter(o => o !== null && typeof o === 'object' && typeof o.label === 'string')
        .map((o, oi) => ({ id: `o${oi}`, label: o.label, description: typeof o.description === 'string' ? o.description : '' })),
    })
  })
  if (parts.length === 0) return null
  return { id, nonce: nonce(), parts }
}

/**
 * Turns the display's answer (option ids per part) into the tool's result. Null when it does not fit the question: a
 * part without an answer, an option id that is not there, several ids for a part that takes one. Such an answer is
 * refused at the integration already; this is the last check before the model reads it.
 */
export function toToolAnswers(question: ReportedQuestion, answer: DisplayAnswer): ToolAnswers | null {
  const answers: ToolAnswers = {}
  for (const part of question.parts) {
    const ids = answer[part.id]
    if (!Array.isArray(ids) || ids.length === 0) return null
    if (!part.multi && ids.length !== 1) return null
    const labels: string[] = []
    for (const id of ids) {
      const option = part.options.find(o => o.id === id)
      if (!option) return null
      labels.push(option.label)
    }
    answers[part.text] = labels.join(', ')
  }
  return answers
}
