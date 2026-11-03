// The launcher's "How did it run?" reports (src/Reports.tsx, src-tauri/src/launcher/reports.rs), and what a game's page
// shows from them. One D1 database; nothing about a player but the PC's parts and an anonymous install id's hash.
//
//   POST /v1/reports   {game, install, verdict, version, mins, note, os, gpu, cpu, ram_gb}  -> {ok: true} | {error}
//                      One report per install, game and version: a second one replaces the first.
//   GET  /v1/reports?game=<id>  -> {total, counts: {runs, issues, broken}, versions: {<v>: counts}, recent: [...]}
//
// A report a moderator hides (UPDATE reports SET hidden = 1 ...) stops counting and showing.
import { env } from "cloudflare:workers";

const VERDICTS = ["runs", "issues", "broken"] as const;
type Verdict = (typeof VERDICTS)[number];
type Counts = Record<Verdict, number>;

const RECENT = 20;
const PER_IP_HOUR = 20;
const BODY_MAX = 4096;
const GAME_ID = /^[a-z0-9_-]{1,48}$/;

const SCHEMA = [
	`CREATE TABLE IF NOT EXISTS reports (
		game TEXT NOT NULL, install TEXT NOT NULL, version TEXT NOT NULL, verdict TEXT NOT NULL,
		mins INTEGER NOT NULL DEFAULT 0, note TEXT NOT NULL DEFAULT '', os TEXT NOT NULL DEFAULT '',
		gpu TEXT NOT NULL DEFAULT '', cpu TEXT NOT NULL DEFAULT '', ram_gb INTEGER NOT NULL DEFAULT 0,
		at INTEGER NOT NULL, hidden INTEGER NOT NULL DEFAULT 0,
		PRIMARY KEY (game, install, version))`,
	`CREATE INDEX IF NOT EXISTS reports_newest ON reports (game, hidden, at DESC)`,
	// Who sent what in the last hour, for the limit: the IP only as a hash salted with the hour, deleted after it.
	`CREATE TABLE IF NOT EXISTS sends (ip TEXT NOT NULL, at INTEGER NOT NULL)`,
	`CREATE INDEX IF NOT EXISTS sends_ip ON sends (ip, at)`,
];

// The tables are made by the Worker itself, once per instance, so a new database (local or deployed) needs no step.
let ready: Promise<unknown> | null = null;
const schema = () => (ready ??= env.DB.batch(SCHEMA.map((s) => env.DB.prepare(s))).catch((e) => ((ready = null), Promise.reject(e))));

const json = (body: unknown, status = 200) =>
	new Response(JSON.stringify(body), {
		status,
		headers: { "content-type": "application/json; charset=utf-8", "access-control-allow-origin": "*", "cache-control": "no-store" },
	});
const refuse = (error: string, status = 400) => json({ error }, status);

// A report's words: printable, one line, at most `limit` characters.
function text(value: unknown, limit: number): string {
	if (typeof value !== "string") return "";
	return value.replace(/[\p{Cc}\p{Cf}\u2028\u2029]+/gu, " ").replace(/\s+/g, " ").trim().slice(0, limit);
}
function whole(value: unknown, low: number, high: number): number {
	const n = typeof value === "number" ? Math.trunc(value) : NaN;
	return Number.isFinite(n) ? Math.min(high, Math.max(low, n)) : 0;
}
async function sha256(s: string): Promise<string> {
	const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
	return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function add(request: Request): Promise<Response> {
	const raw = await request.text();
	if (raw.length > BODY_MAX) return refuse("The report is too long", 413);
	let doc: Record<string, unknown>;
	try {
		const v = JSON.parse(raw);
		if (!v || typeof v !== "object" || Array.isArray(v)) throw new Error();
		doc = v;
	} catch {
		return refuse("The report isn't JSON");
	}
	const game = typeof doc.game === "string" ? doc.game : "";
	const verdict = doc.verdict as Verdict;
	const install = text(doc.install, 64);
	if (!GAME_ID.test(game)) return refuse("Bad game id");
	if (!VERDICTS.includes(verdict)) return refuse("Unknown verdict");
	if (install.length < 16) return refuse("Missing install id");

	await schema();
	const now = Math.floor(Date.now() / 1000);
	const hour = Math.floor(now / 3600);
	const ip = (await sha256(`${hour}:${request.headers.get("cf-connecting-ip") ?? "local"}`)).slice(0, 32);
	const sent = await env.DB.prepare("SELECT COUNT(*) AS n FROM sends WHERE ip = ?1 AND at > ?2").bind(ip, now - 3600).first<{ n: number }>();
	if ((sent?.n ?? 0) >= PER_IP_HOUR) return refuse("Too many reports from here; try again later", 429);

	await env.DB.batch([
		env.DB.prepare("DELETE FROM sends WHERE at <= ?1").bind(now - 3600),
		env.DB.prepare("INSERT INTO sends (ip, at) VALUES (?1, ?2)").bind(ip, now),
		// A new answer from the same PC for the same version replaces the old one, and shows again if it was hidden.
		env.DB.prepare(
			`INSERT INTO reports (game, install, version, verdict, mins, note, os, gpu, cpu, ram_gb, at, hidden)
			 VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, 0)
			 ON CONFLICT (game, install, version) DO UPDATE SET verdict = excluded.verdict, mins = excluded.mins,
			   note = excluded.note, os = excluded.os, gpu = excluded.gpu, cpu = excluded.cpu, ram_gb = excluded.ram_gb,
			   at = excluded.at, hidden = 0`,
		).bind(
			game, (await sha256(install)).slice(0, 32), text(doc.version, 32), verdict, whole(doc.mins, 0, 100000),
			text(doc.note, 500), text(doc.os, 48), text(doc.gpu, 64), text(doc.cpu, 64), whole(doc.ram_gb, 0, 4096), now,
		),
	]);
	return json({ ok: true });
}

async function summary(game: string): Promise<Response> {
	if (!GAME_ID.test(game)) return refuse("Bad game id");
	await schema();
	const counts: Counts = { runs: 0, issues: 0, broken: 0 };
	const versions: Record<string, Counts> = {};
	const tally = await env.DB.prepare("SELECT version, verdict, COUNT(*) AS n FROM reports WHERE game = ?1 AND hidden = 0 GROUP BY version, verdict")
		.bind(game).all<{ version: string; verdict: Verdict; n: number }>();
	for (const r of tally.results) {
		if (!VERDICTS.includes(r.verdict)) continue;
		counts[r.verdict] += r.n;
		(versions[r.version] ??= { runs: 0, issues: 0, broken: 0 })[r.verdict] += r.n;
	}
	const recent = await env.DB.prepare(
		"SELECT verdict, version, mins, note, os, gpu, cpu, ram_gb, at FROM reports WHERE game = ?1 AND hidden = 0 ORDER BY at DESC, rowid DESC LIMIT ?2",
	).bind(game, RECENT).all();
	return json({ total: counts.runs + counts.issues + counts.broken, counts, versions, recent: recent.results });
}

export default {
	async fetch(request) {
		const url = new URL(request.url);
		if (url.pathname !== "/v1/reports") return refuse("Not found", 404);
		if (request.method === "GET") return summary(url.searchParams.get("game") ?? "");
		if (request.method === "POST") return add(request);
		if (request.method === "OPTIONS") return new Response(null, { status: 204, headers: { "access-control-allow-origin": "*", "access-control-allow-methods": "GET" } });
		return refuse("Method not allowed", 405);
	},
} satisfies ExportedHandler;
