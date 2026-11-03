/** A tool's name as a person reads it: an MCP tool without its server id prefix. */
export function toolLabel(tool: string): string {
  const mcp = /^mcp__(.+?)__(.+)$/.exec(tool)
  if (!mcp) return tool
  const raw = mcp[1] ?? ''
  // A connector's server is named by a UUID, which says nothing to a person.
  const isUuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(raw)
  const server = isUuid ? '' : raw
  return server ? `${mcp[2]} (${server})` : (mcp[2] ?? tool)
}

/** One line that says what a tool call is about, for the permission hint. */
export function describeCall(tool: string, input: unknown): string {
  const i = (input ?? {}) as Record<string, unknown>
  const pick = (key: string) => (typeof i[key] === 'string' ? (i[key] as string) : undefined)
  const label = toolLabel(tool)
  const domain = pick('domain')
  const service = pick('service')
  const detail =
    pick('command') ??
    pick('description') ??
    pick('file_path') ??
    pick('path') ??
    pick('url') ??
    pick('pattern') ??
    (domain && service ? `${domain}.${service}${pick('entity_id') ? ' → ' + pick('entity_id') : ''}` : undefined) ??
    pick('entity_id') ??
    pick('event_type') ??
    pick('query')
  if (detail) return `${label}: ${detail.replace(/\s+/g, ' ')}`
  const keys = Object.keys(i)
  return keys.length > 0 ? `${label}: ${keys.slice(0, 4).join(', ')}` : label
}

/** The last path segment of a directory, for a session's name. */
export function baseName(path: string): string {
  const parts = path.replace(/[\\/]+$/, '').split(/[\\/]/)
  return parts[parts.length - 1] || path
}
