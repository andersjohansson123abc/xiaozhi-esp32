// Relay between the FoloToy AI Passport and Grok Bots.
//
// Grok Bot has no public API, and the Passport and the Bots cannot reach each
// other directly, so this Worker sits between them:
//
//   Passport -> /device/send -> the Bot's routine webhook
//   Bot      -> /mcp tools   -> stored here -> /device/poll -> Passport
//
// The webhook keys stay here, so a lost Passport costs one rotated DEVICE_TOKEN.
// A Bot's status is what the Bot reported. The only states derived here are
// `noreply` and `quiet`, and they exist to show when a Bot has stopped reporting.

const PROTOCOL = "GROK1";
const MAX_BOTS = 6;
const MAX_MESSAGES_PER_POLL = 3;
const MAX_NOTE_CHARS = 48;
const MAX_REPLY_CHARS = 400;
const MAX_MESSAGE_CHARS = 800;
const MAX_BODY_BYTES = 8192;
const WEBHOOK_TIMEOUT_MS = 10000;
const KEEP_MESSAGES_SECONDS = 7 * 24 * 3600;
const REPORTED_STATES = ["idle", "working", "waiting", "error"];
const MCP_DEFAULT_VERSION = "2025-06-18";

const MCP_TOOLS = [
  {
    name: "passport_report_status",
    description:
      "Report what you are doing to the owner's FoloToy AI Passport. Call it when you start " +
      "work, when you stop to wait for an approval, and when you finish or fail.",
    inputSchema: {
      type: "object",
      properties: {
        bot_id: { type: "string", description: "Your id on the Passport." },
        state: { type: "string", enum: REPORTED_STATES },
        note: { type: "string", description: `At most ${MAX_NOTE_CHARS} characters of Swedish.` },
      },
      required: ["bot_id", "state"],
    },
  },
  {
    name: "passport_reply",
    description:
      "Answer the owner on their FoloToy AI Passport. The text is read aloud, so write short " +
      `spoken Swedish with no lists or links, at most ${MAX_REPLY_CHARS} characters.`,
    inputSchema: {
      type: "object",
      properties: {
        bot_id: { type: "string", description: "Your id on the Passport." },
        text: { type: "string" },
      },
      required: ["bot_id", "text"],
    },
  },
];

function text(body, status = 200) {
  return new Response(body, {
    status,
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" },
  });
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

function intVar(value, fallback) {
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : fallback;
}

// Compares every byte, so the response time says nothing about the token.
function sameToken(given, expected) {
  if (typeof given !== "string" || !expected) {
    return false;
  }
  const a = new TextEncoder().encode(given);
  const b = new TextEncoder().encode(expected);
  let difference = a.length ^ b.length;
  for (let i = 0; i < b.length; i++) {
    difference |= (a[i] ?? 0) ^ b[i];
  }
  return difference === 0;
}

function bearer(request) {
  const header = request.headers.get("authorization") ?? "";
  return header.startsWith("Bearer ") ? header.slice(7) : "";
}

// Everything a Bot writes ends up in one tab-separated line on a small display,
// and the display has no glyphs for emoji.
function clean(value, maxChars) {
  const flat = String(value ?? "")
    .replace(/[\p{Cc}\p{Zl}\p{Zp}]+/gu, " ")
    .replace(/[\p{Extended_Pictographic}\p{Cf}\p{Variation_Selector}]/gu, "")
    .replace(/ {2,}/g, " ")
    .trim();
  const chars = Array.from(flat);
  return { text: chars.slice(0, maxChars).join(""), truncated: chars.length > maxChars };
}

function loadBots(env) {
  let parsed;
  try {
    parsed = JSON.parse(env.BOTS ?? "[]");
  } catch {
    throw new Error("BOTS is not valid JSON");
  }
  if (!Array.isArray(parsed) || parsed.length === 0 || parsed.length > MAX_BOTS) {
    throw new Error(`BOTS must list 1-${MAX_BOTS} bots`);
  }
  for (const bot of parsed) {
    if (!/^[a-z0-9-]{1,16}$/.test(bot.id ?? "") || !bot.name || !bot.webhook || !bot.key) {
      throw new Error("Each bot needs id (a-z, 0-9, -), name, webhook and key");
    }
    if (!bot.webhook.startsWith("https://")) {
      throw new Error(`The webhook of ${bot.id} must use https`);
    }
  }
  return parsed;
}

export function effectiveState(row, now, env) {
  const noReply = intVar(env.NO_REPLY_SECONDS, 900);
  const stale = intVar(env.STALE_SECONDS, 7200);
  if (row?.send_failed) {
    return { state: "error", note: "Webhooken nekade" };
  }
  if (row?.sent_at && row.sent_at > (row.replied_at ?? 0) && now - row.sent_at >= noReply) {
    return { state: "noreply", note: "Har inte svarat" };
  }
  if (!row?.reported_at) {
    return { state: "unknown", note: "" };
  }
  if ((row.state === "working" || row.state === "waiting") && now - row.reported_at >= stale) {
    return { state: "quiet", note: row.note ?? "" };
  }
  return { state: row.state, note: row.note ?? "" };
}

async function handlePoll(request, env, now) {
  const ack = new URL(request.url).searchParams.get("ack");
  if (ack !== null) {
    if (!/^\d{1,15}$/.test(ack)) {
      return text("bad ack", 400);
    }
    await env.DB.prepare("UPDATE messages SET read_at = ? WHERE id <= ? AND read_at IS NULL")
      .bind(now, Number(ack))
      .run();
  }

  const bots = loadBots(env);
  const { results: rows } = await env.DB.prepare("SELECT * FROM bots").all();
  const byId = new Map(rows.map((row) => [row.id, row]));
  const { results: unread } = await env.DB.prepare(
    "SELECT id, bot_id, text, created_at FROM messages WHERE read_at IS NULL ORDER BY id LIMIT ?",
  )
    .bind(MAX_MESSAGES_PER_POLL)
    .all();
  const { count } = await env.DB.prepare(
    "SELECT COUNT(*) AS count FROM messages WHERE read_at IS NULL",
  ).first();

  const lines = [PROTOCOL, `U\t${count}`];
  for (const bot of bots) {
    const row = byId.get(bot.id);
    const { state, note } = effectiveState(row, now, env);
    const age = row?.reported_at ? now - row.reported_at : -1;
    const name = clean(bot.name, 20).text;
    lines.push(`B\t${bot.id}\t${name}\t${state}\t${age}\t${clean(note, MAX_NOTE_CHARS).text}`);
  }
  for (const message of unread) {
    lines.push(`M\t${message.id}\t${message.bot_id}\t${now - message.created_at}\t${message.text}`);
  }
  return text(lines.join("\n") + "\n");
}

async function handleSend(request, env, now) {
  const form = new URLSearchParams(await request.text());
  const bot = loadBots(env).find((candidate) => candidate.id === form.get("bot"));
  if (!bot) {
    return text("unknown bot", 404);
  }
  const message = clean(form.get("text"), MAX_MESSAGE_CHARS).text;
  if (!message) {
    return text("empty message", 400);
  }

  // Each run spends the owner's Grok usage, and a misheard loop should not.
  const limit = intVar(env.MAX_SENDS_PER_HOUR, 30);
  const { count } = await env.DB.prepare("SELECT COUNT(*) AS count FROM sends WHERE at > ?")
    .bind(now - 3600)
    .first();
  if (count >= limit) {
    return text("too many messages this hour", 429);
  }
  await env.DB.prepare("INSERT INTO sends (bot_id, at) VALUES (?, ?)").bind(bot.id, now).run();
  await env.DB.prepare("DELETE FROM sends WHERE at <= ?").bind(now - 3600).run();
  await env.DB.prepare("INSERT INTO bots (id) VALUES (?) ON CONFLICT (id) DO NOTHING")
    .bind(bot.id)
    .run();

  let accepted = false;
  try {
    const response = await fetch(bot.webhook, {
      method: "POST",
      headers: { authorization: `Bearer ${bot.key}`, "content-type": "application/json" },
      body: JSON.stringify({ source: "passport", bot_id: bot.id, message }),
      signal: AbortSignal.timeout(WEBHOOK_TIMEOUT_MS),
    });
    // A 200 means that a run started, and nothing else.
    accepted = response.status === 200;
  } catch (error) {
    console.error(`Webhook of ${bot.id} failed: ${error.name}`);
  }

  await env.DB.prepare("UPDATE bots SET send_failed = ?, sent_at = ? WHERE id = ?")
    .bind(accepted ? 0 : 1, accepted ? now : null, bot.id)
    .run();
  return accepted ? text("sent") : text("webhook refused", 502);
}

function knownBot(env, id) {
  return loadBots(env).find((bot) => bot.id === id);
}

function toolResult(message, isError = false) {
  return { content: [{ type: "text", text: message }], isError };
}

async function callTool(params, env, now) {
  const args = params?.arguments ?? {};
  const bot = knownBot(env, args.bot_id);
  if (!bot) {
    const ids = loadBots(env).map((candidate) => candidate.id).join(", ");
    return toolResult(`Unknown bot_id. Valid ids: ${ids}`, true);
  }

  if (params?.name === "passport_report_status") {
    if (!REPORTED_STATES.includes(args.state)) {
      return toolResult(`state must be one of: ${REPORTED_STATES.join(", ")}`, true);
    }
    const note = clean(args.note, MAX_NOTE_CHARS).text;
    await env.DB.prepare(
      "INSERT INTO bots (id, state, note, reported_at) VALUES (?1, ?2, ?3, ?4) " +
        "ON CONFLICT (id) DO UPDATE SET state = ?2, note = ?3, reported_at = ?4",
    )
      .bind(bot.id, args.state, note, now)
      .run();
    return toolResult("Status saved.");
  }

  if (params?.name === "passport_reply") {
    const { text: reply, truncated } = clean(args.text, MAX_REPLY_CHARS);
    if (!reply) {
      return toolResult("text is empty", true);
    }
    await env.DB.prepare("INSERT INTO messages (bot_id, text, created_at) VALUES (?, ?, ?)")
      .bind(bot.id, reply, now)
      .run();
    await env.DB.prepare(
      "INSERT INTO bots (id, state, note, reported_at, replied_at) VALUES (?1, 'idle', '', ?2, ?2) " +
        "ON CONFLICT (id) DO UPDATE SET state = 'idle', note = '', reported_at = ?2, replied_at = ?2",
    )
      .bind(bot.id, now)
      .run();
    await env.DB.prepare("DELETE FROM messages WHERE read_at IS NOT NULL AND read_at < ?")
      .bind(now - KEEP_MESSAGES_SECONDS)
      .run();
    return toolResult(
      truncated
        ? `Delivered, but cut to ${MAX_REPLY_CHARS} characters. The full answer stays in this chat.`
        : "Delivered.",
    );
  }

  return toolResult("Unknown tool", true);
}

async function handleMcp(request, env, now) {
  let call;
  try {
    call = JSON.parse(await request.text());
  } catch {
    return json({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "Parse error" } }, 400);
  }
  // Notifications carry no id and take no answer.
  if (call.id === undefined) {
    return new Response(null, { status: 202 });
  }

  const answer = (result) => json({ jsonrpc: "2.0", id: call.id, result });
  switch (call.method) {
    case "initialize":
      return answer({
        protocolVersion: call.params?.protocolVersion ?? MCP_DEFAULT_VERSION,
        capabilities: { tools: {} },
        serverInfo: { name: "passport-relay", version: "1.0.0" },
      });
    case "ping":
      return answer({});
    case "tools/list":
      return answer({ tools: MCP_TOOLS });
    case "tools/call":
      return answer(await callTool(call.params, env, now));
    default:
      return json({
        jsonrpc: "2.0",
        id: call.id,
        error: { code: -32601, message: "Method not found" },
      });
  }
}

async function route(request, env, now) {
  const { pathname } = new URL(request.url);
  const length = Number(request.headers.get("content-length") ?? 0);
  if (length > MAX_BODY_BYTES) {
    return text("too large", 413);
  }

  if (pathname === "/device/poll" || pathname === "/device/send") {
    if (!sameToken(bearer(request), env.DEVICE_TOKEN)) {
      return text("unauthorized", 401);
    }
    if (pathname === "/device/poll") {
      return request.method === "GET" ? handlePoll(request, env, now) : text("method", 405);
    }
    return request.method === "POST" ? handleSend(request, env, now) : text("method", 405);
  }

  if (pathname === "/mcp" || pathname.startsWith("/mcp/")) {
    // A Bot adds an MCP server by URL, so the token may also be the last segment.
    const token = pathname === "/mcp" ? bearer(request) : pathname.slice("/mcp/".length);
    if (!sameToken(token, env.BOT_TOKEN)) {
      return text("unauthorized", 401);
    }
    return request.method === "POST" ? handleMcp(request, env, now) : text("method", 405);
  }

  return text("not found", 404);
}

export default {
  async fetch(request, env) {
    try {
      return await route(request, env, Math.floor(Date.now() / 1000));
    } catch (error) {
      console.error(error.message);
      return text("relay error", 500);
    }
  },
};
