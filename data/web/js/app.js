import { api, all, session, may, key, osdUrl, ApiError, pairOnTv, httpsUrl, playerUrl, applyTheme, loadTexts, language, tr, trn, trx } from "./api.js";
import { loadLayout, drawRemote, KEYBOARD } from "./remote.js";

const $ = (sel, root = document) => root.querySelector(sel);

const ICONS = {
	remote: '<rect x="7" y="2" width="10" height="20" rx="3"/><circle cx="12" cy="9" r="2.2"/><path d="M10 15h.01M14 15h.01M10 18h.01M14 18h.01"/>',
	channels: '<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8M12 17v4"/>',
	epg: '<rect x="3" y="4" width="18" height="17" rx="2"/><path d="M3 9h18M8 2v4M16 2v4M7 13h4M7 17h7"/>',
	timers: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2.5 2.5M9 2h6"/>',
	recordings: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M3 9h18M7 5v4M12 5v4M17 5v4M10 12.5v4l3.5-2z"/>',
	mounts: '<rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M7 7.5h.01M7 16.5h.01M11 7.5h6M11 16.5h6"/>',
	box: '<rect x="2" y="7" width="20" height="10" rx="2"/><path d="M6 12h.01M10 12h4"/>',
	record: '<circle cx="12" cy="12" r="6" fill="currentColor" stroke="none"/>',
	stream: '<path d="M8 5v14l11-7z" fill="currentColor" stroke="none"/>',
	trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
	series: '<path d="M17 2l4 4-4 4M3 11V9a3 3 0 0 1 3-3h15M7 22l-4-4 4-4M21 13v2a3 3 0 0 1-3 3H3"/>',
	refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7"/>',
	phone: '<rect x="7" y="2" width="10" height="20" rx="2"/><path d="M11 18h2"/>',
	camera: '<path d="M4 8h3l1.5-2.5h7L17 8h3a1 1 0 0 1 1 1v9.5a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1z"/><circle cx="12" cy="13.5" r="3.5"/>',
	cloud: '<path d="M7 18h10.5a4 4 0 0 0 .5-7.97A6 6 0 0 0 6.34 9.5 4.25 4.25 0 0 0 7 18z"/>',
	puzzle: '<path d="M10 4a2 2 0 1 1 4 0v2h4v4h-2a2 2 0 1 0 0 4h2v4h-4v-2a2 2 0 1 0-4 0v2H6v-4H4a2 2 0 1 1 0-4h2V6h4z"/>',
};

function icon(name) {
	return `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${ICONS[name] || ICONS.puzzle}</svg>`;
}

function h(tag, attrs = {}, ...children) {
	const e = document.createElement(tag);
	for (const [k, v] of Object.entries(attrs)) {
		if (k.startsWith("on"))
			e.addEventListener(k.slice(2), v);
		else if (k === "html")
			e.innerHTML = v;
		else if (v !== false && v !== undefined && v !== null)
			e.setAttribute(k, v === true ? "" : v);
	}
	for (const c of children.flat())
		if (c !== null && c !== undefined && c !== false)
			e.append(c instanceof Node ? c : document.createTextNode(String(c)));
	return e;
}

let toastTimer = 0;
export function toast(text, error = false) {
	let t = $(".toast");
	if (!t) {
		t = h("div", { class: "toast", role: "status" });
		document.body.append(t);
	}
	t.textContent = text;
	t.classList.toggle("error", error);
	t.hidden = false;
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
}

function fail(e) {
	if (e instanceof ApiError && (e.status === 401 || e.status === 403)) {
		location.hash = "#/signin";
		return;
	}
	toast(e.message || String(e), true);
}

function confirmDialog(text, okText = tr("ok"), danger = false) {
	return new Promise((resolve) => {
		const d = h("dialog", {},
			h("p", {}, text),
			h("div", { class: "buttons" },
				h("button", { class: "btn", value: "no", onclick: () => d.close("no") }, tr("cancel")),
				h("button", { class: `btn ${danger ? "danger" : "primary"}`, onclick: () => d.close("yes") }, okText)));
		d.addEventListener("close", () => { d.remove(); resolve(d.returnValue === "yes"); });
		document.body.append(d);
		d.showModal();
	});
}

const pad = (n) => String(n).padStart(2, "0");
const hm = (t) => { const d = new Date(t * 1000); return `${pad(d.getHours())}:${pad(d.getMinutes())}`; };
const day = (t) => new Date(t * 1000).toLocaleDateString(language(), { weekday: "short", day: "numeric", month: "short" });
const now = () => Date.now() / 1000;

/* ------------------------------------------------------------------ */
/* shared data                                                         */

const cache = { bouquets: null, names: new Map() };

async function tvBouquets() {
	if (!cache.bouquets) {
		const items = (await api("bouquets")).items || [];
		cache.bouquets = items.filter((b) => !b.hidden && b.tv_count > 0);
	}
	return cache.bouquets;
}

async function channelName(id) {
	if (!cache.names.has(id)) {
		try {
			cache.names.set(id, (await api(`channels/${id}`)).name);
		} catch {
			cache.names.set(id, id);
		}
	}
	return cache.names.get(id);
}

function bouquetSelect(bouquets, selected, onchange) {
	const s = h("select", { "aria-label": tr("bouquet"), onchange: () => onchange(s.value) },
		bouquets.map((b) => h("option", { value: b.id, selected: String(b.id) === String(selected) }, b.name)));
	return s;
}

function savedBouquet(bouquets) {
	let id = "";
	try { id = localStorage.getItem("bouquet") || ""; } catch { /* no storage */ }
	return bouquets.some((b) => String(b.id) === id) ? id : String(bouquets[0]?.id ?? "");
}

function saveBouquet(id) {
	try { localStorage.setItem("bouquet", id); } catch { /* no storage */ }
}

async function record(channel, ev) {
	const body = { kind: "record", channel_id: channel, start: ev.start, stop: ev.start + ev.duration,
		title: ev.title, recording_safety: true };
	if (ev.id) {
		body.epg_id = ev.id;
		body.epg_start = ev.start;
	}
	let r;
	try {
		r = await api("timers", { method: "POST", body });
	} catch (e) {
		/* the box sees no tuner for it beside the timers there are, as its own menus ask */
		if (!(e instanceof ApiError && e.code === "timer-conflict"))
			throw e;
		const what = (e.problem.conflicts || []).map((c) => `"${c.title}" ${hm(c.start)}-${hm(c.stop)}`).join(", ");
		if (!await confirmDialog(tr("timer.conflict", { title: ev.title, other: what }), tr("timer.set_anyway")))
			return null;
		r = await api("timers", { method: "POST", body: { ...body, force: true } });
	}
	toast(tr("timer.recording_set", { title: ev.title }));
	return r;
}

/* a series timer: the rule that records every broadcast of a title on a
 * channel, also those the guide gets later */
/* the series in a title, as the box takes it: without the number of the
 * episode, "Hubert ohne Staller (199)", "Lost Places - Folge 119" */
const seriesName = (title) => {
	let t = String(title).trim();
	for (let before; before !== t;) {
		before = t;
		t = t.replace(/\s*\(\d+(\/\d+)?\)$/, "").replace(/\s*[-–:,]?\s*Folge\s*\d+$/i, "").trim();
	}
	return (t || String(title).trim()).toLowerCase();
};
const sameSeries = (rule, channel, title) => rule.channel_id === channel && seriesName(rule.title) === seriesName(title);
const seriesRules = () => api("timers/series").then((r) => r.items || [], () => []);

async function recordSeries(channel, ev) {
	const r = await api("timers/series", { method: "POST", body: { title: ev.title, channel_id: channel } });
	toast([trn("series.set", r.added), r.conflicts ? trn("series.conflicts", r.conflicts) : ""].filter(Boolean).join(" - "));
	return r;
}

/* the browser runs on the box itself, where a stream would only play
 * what the TV plays already */
const onBox = ["localhost", "127.0.0.1", "[::1]", "::1"].includes(location.hostname);

async function zap(ch) {
	await api("zap", { method: "POST", body: { channel_id: ch.id, stop_playback: true } });
	toast(tr("zapped", { name: ch.name }));
}

/* An Android phone hands the stream to a video player app; elsewhere an
 * mpv: link starts mpv, where a handler for it is set up (mpv-url.desktop).
 * The address goes to the clipboard as well, for when nothing opens. */
async function stream(id) {
	await playUrl((await api(`stream/${id}`)).url);
}

async function playUrl(url) {
	const s = { url: playerUrl(url) };
	const m = s.url.match(/^(https?):\/\/(.*)$/);
	if (/Android/i.test(navigator.userAgent) && m) {
		location.href = `intent://${m[2]}#Intent;scheme=${m[1]};action=android.intent.action.VIEW;type=video/*;end`;
		return;
	}
	let copied = false;
	try {
		await navigator.clipboard.writeText(s.url);
		copied = true;
	} catch { /* no clipboard here */ }
	location.href = `mpv:${encodeURIComponent(s.url)}`;
	toast(copied ? tr("mpv.clipboard") : tr("mpv.start"));
}

/* ------------------------------------------------------------------ */
/* views                                                               */

let cleanup = null;

async function viewRemote(main) {
	const osd = h("div", { class: "osd" }, h("span", { class: "hint" }, tr("remote.osd_hint")));
	const pad = h("div", { class: "remote" });
	main.append(h("h1", {}, tr("remote.title")),
		h("div", { class: "remote-view" }, pad,
			h("div", { class: "osd-col" }, osd,
				h("p", { class: "dim small" }, trx("remote.keyboard", { link: h("a", { href: "/remote.html" }, tr("remote.only")) })))));

	/* the box answers a key before it paints, so the OSD is fetched right
	 * away and again while it settles; one request at a time, and a picture
	 * never replaces a newer one */
	let timers = [];
	let busy = false, again = false, asked = 0, shown = 0;
	const shot = () => {
		if (busy) {
			again = true;
			return;
		}
		busy = true;
		again = false;
		const n = ++asked;
		const img = new Image();
		const done = () => {
			busy = false;
			if (again)
				shot();
		};
		img.onload = () => {
			if (n > shown) {
				shown = n;
				const c = document.createElement("canvas");
				c.width = 32; c.height = 18;
				const g = c.getContext("2d");
				g.drawImage(img, 0, 0, 32, 18);
				const empty = !g.getImageData(0, 0, 32, 18).data.some((v, i) => i % 4 === 3 && v > 0);
				osd.replaceChildren(empty ? h("span", { class: "hint" }, tr("remote.no_menu")) : img);
			}
			done();
		};
		img.onerror = done;
		img.src = osdUrl();
	};
	const follow = () => {
		timers.forEach(clearTimeout);
		shot();
		timers = [150, 450, 2000].map((t) => setTimeout(shot, t));
	};
	const press = (name) => key(name).then(follow, fail);

	drawRemote(pad, await loadLayout(), press);
	const onkey = (ev) => {
		if (ev.target.closest("input, textarea, select") || ev.ctrlKey || ev.metaKey || ev.altKey)
			return;
		const name = KEYBOARD[ev.key];
		if (name) {
			ev.preventDefault();
			press(name);
		}
	};
	document.addEventListener("keydown", onkey);
	cleanup = () => {
		document.removeEventListener("keydown", onkey);
		timers.forEach(clearTimeout);
	};
}

async function viewChannels(main) {
	const bouquets = await tvBouquets();
	if (!bouquets.length) {
		main.append(h("div", { class: "empty" }, tr("channels.none")));
		return;
	}
	const list = h("div", { class: "list" });
	let current = "";
	const show = async (id) => {
		saveBouquet(id);
		list.replaceChildren(h("div", { class: "empty" }, tr("loading")));
		const from = Math.floor(now());
		const [channels, grid, cur] = await Promise.all([
			all(`channels?bouquet=${id}&limit=500`),
			all(`epg/grid?bouquet=${id}&from=${from}&to=${from + 6 * 3600}&limit=100`).catch(() => []),
			api("channels/current").catch(() => null),
		]);
		current = cur?.id || "";
		const events = new Map(grid.map((r) => [r.channel_id, r.events || []]));
		const t = now();
		list.replaceChildren(...channels.map((ch) => {
			const evs = (events.get(ch.id) || []).filter((e) => e.start + e.duration > t);
			const nowEv = evs.find((e) => e.start <= t);
			const nextEv = evs.find((e) => e.start > t);
			const pct = nowEv ? Math.min(100, Math.max(0, (t - nowEv.start) / nowEv.duration * 100)) : 0;
			const logo = h("img", { class: "logo", src: `/api/v1/channels/${ch.id}/logo`, alt: "", loading: "lazy" });
			logo.onerror = () => { logo.style.visibility = "hidden"; };
			const row = h("div", { class: `channel${ch.id === current ? " current" : ""}`, title: nowEv?.description || "" },
				h("span", { class: "num" }, ch.number),
				logo,
				h("div", { class: "info" },
					h("div", { class: "name" }, ch.name),
					nowEv ? h("div", { class: "now" }, `${hm(nowEv.start)} ${nowEv.title}`) : h("div", { class: "now dim" }, tr("channels.no_epg")),
					/* hidden where there is nothing to show, so that every row is as high */
					h("div", { class: "progress", style: nowEv ? "" : "visibility:hidden" }, h("span", { style: `width:${pct}%` })),
					h("div", { class: "next", style: nextEv ? "" : "visibility:hidden" }, nextEv ? `${hm(nextEv.start)} ${nextEv.title}` : "-")),
				h("div", { class: "actions" },
					may("write") && nowEv ? h("button", { class: "btn icon", title: tr("channels.record"), html: icon("record"),
						onclick: (ev) => { ev.stopPropagation(); record(ch.id, nowEv).catch(fail); } }) : null,
					onBox ? null : h("button", { class: "btn icon", title: tr("channels.stream"), html: icon("stream"),
						onclick: (ev) => { ev.stopPropagation(); stream(ch.id).catch(fail); } })));
			row.addEventListener("click", async () => {
				try {
					await api("zap", { method: "POST", body: { channel_id: ch.id, stop_playback: true } });
					list.querySelector(".channel.current")?.classList.remove("current");
					row.classList.add("current");
					toast(tr("zapped", { name: ch.name }));
				} catch (e) { fail(e); }
			});
			return row;
		}));
		list.querySelector(".channel.current")?.scrollIntoView({ block: "center" });
	};
	const sel = bouquetSelect(bouquets, savedBouquet(bouquets), (id) => show(id).catch(fail));
	main.append(h("div", { class: "toolbar" }, h("h1", {}, tr("nav.channels")), sel,
		h("button", { class: "btn icon", title: tr("reload"), html: icon("refresh"), onclick: () => show(sel.value).catch(fail) })), list);
	await show(sel.value);
}

const PX_PER_MIN = 4;

/* the long description of a programme, asked of the box once */
const longTexts = new Map();
function longText(ev) {
	if (!ev.id)
		return Promise.resolve("");
	if (!longTexts.has(ev.id))
		longTexts.set(ev.id, api(`epg/events/${ev.id}?start=${ev.start}`).then((e) => e.info2 || "", () => ""));
	return longTexts.get(ev.id);
}

/* a text cut at a word, for a tooltip */
function shortText(text, max) {
	if (text.length <= max)
		return text;
	const cut = text.lastIndexOf(" ", max);
	return `${text.slice(0, cut > max / 2 ? cut : max)} …`;
}

/* el is the programme in the guide; el.timerId the recording set for it.
 * series: the rules of the series timers and how to draw the guide anew
 * once a rule has set its timers */
function eventDialog(ch, ev, el, series) {
	const end = ev.start + ev.duration;
	const running = ev.start <= now() && now() < end;
	const timerId = el?.timerId;
	let timerButton = null;
	if (may("write") && timerId !== undefined)
		timerButton = h("button", { class: "btn danger", onclick: async () => {
			d.close();
			/* a timer to come is set again as fast; a recording stopped is gone */
			if (running && !await confirmDialog(tr("timer.stop_ask", { title: ev.title }), tr("timer.stop"), true))
				return;
			try {
				await api(`timers/${timerId}`, { method: "DELETE" });
				el.classList.remove("timer");
				delete el.timerId;
				toast(running ? tr("timer.stopped", { title: ev.title }) : tr("timer.deleted", { title: ev.title }));
			} catch (e) { fail(e); }
		} }, running ? tr("timer.stop") : tr("timer.delete"));
	else if (may("write"))
		timerButton = h("button", { class: "btn primary", onclick: () => {
			record(ch.id, ev).then((r) => {
				if (el && r?.id !== undefined) {
					el.classList.add("timer");
					el.timerId = r.id;
				}
			}).catch(fail);
			d.close();
		} }, tr("record"));
	let seriesButton = null;
	const rule = series?.rules.find((r) => sameSeries(r, ch.id, ev.title));
	if (may("write") && rule)
		seriesButton = h("button", { class: "btn danger", onclick: async () => {
			d.close();
			try {
				await api(`timers/series/${rule.id}`, { method: "DELETE" });
				series.rules.splice(series.rules.indexOf(rule), 1);
				toast(tr("series.deleted"));
			} catch (e) { fail(e); }
		} }, tr("series.delete"));
	else if (may("write") && series)
		seriesButton = h("button", { class: "btn", title: tr("series.hint"), onclick: () => {
			d.close();
			recordSeries(ch.id, ev).then((r) => {
				series.rules.push(r);
				return series.refresh();
			}).catch(fail);
		} }, tr("series.record_all"));
	const d = h("dialog", {},
		h("h2", {}, ev.title),
		h("p", { class: "dim small" }, `${ch.name} - ${day(ev.start)} ${hm(ev.start)}-${hm(ev.start + ev.duration)}`),
		ev.description ? h("p", {}, ev.description) : null,
		h("div", { class: "buttons" },
			h("button", { class: "btn", onclick: () => d.close() }, tr("close")),
			onBox ? null : h("button", { class: "btn", onclick: () => { stream(ch.id).catch(fail); d.close(); } }, tr("stream")),
			may("write") ? h("button", { class: "btn", onclick: () => { zap(ch).catch(fail); d.close(); } }, tr("zap")) : null,
			seriesButton, timerButton));
	d.addEventListener("close", () => d.remove());
	document.body.append(d);
	d.showModal();
}

async function viewEpg(main) {
	const bouquets = await tvBouquets();
	if (!bouquets.length) {
		main.append(h("div", { class: "empty" }, tr("channels.none")));
		return;
	}
	const box = h("div", { class: "epg" });
	const show = async (id) => {
		saveBouquet(id);
		box.replaceChildren(h("div", { class: "empty" }, tr("loading")));
		const start = Math.floor(now() / 1800) * 1800;
		/* as many hours as the window shows, and two more to scroll to */
		const hours = Math.min(24, Math.max(8, Math.ceil((box.clientWidth - 150) / (60 * PX_PER_MIN)) + 2));
		const [channels, grid, timers, rules] = await Promise.all([
			all(`channels?bouquet=${id}&limit=500`),
			all(`epg/grid?bouquet=${id}&from=${start}&to=${start + hours * 3600}&limit=100`),
			api("timers").then((r) => r.items || [], () => []),
			seriesRules(),
		]);
		/* a new rule has set timers: the guide anew, where it was scrolled to */
		const series = { rules, refresh: async () => {
			const [left, top] = [box.scrollLeft, box.scrollTop];
			await show(id);
			box.scrollLeft = left;
			box.scrollTop = top;
		} };
		/* a programme a recording is set for: by its event, or covered more than half */
		const records = timers.filter((tm) => tm.kind === "record");
		const recordFor = (ch, ev, end) => records.find((tm) => tm.channel_id === ch.id &&
			((tm.epg_id && tm.epg_id === ev.id) || Math.min(tm.stop, end) - Math.max(tm.start, ev.start) > (end - ev.start) / 2));
		const events = new Map(grid.map((r) => [r.channel_id, r.events || []]));
		const width = hours * 60 * PX_PER_MIN;
		const x = (t) => (t - start) / 60 * PX_PER_MIN;
		const times = h("div", { class: "times", style: `margin-left:150px;width:${width}px` });
		for (let t = start; t < start + hours * 3600; t += 1800)
			times.append(h("span", { style: `left:${x(t) + 4}px` }, hm(t)));
		const grid_el = h("div", { class: "grid", style: `width:${width + 150}px` }, times);
		const t = now();
		for (const ch of channels) {
			const line = h("div", { class: "events", style: `width:${width}px` });
			/* the guide may let one programme run into the next; each
			 * ends where the next one starts, so none covers another */
			const evs = (events.get(ch.id) || []).slice().sort((a, b) => a.start - b.start);
			evs.forEach((ev, i) => {
				const next = evs[i + 1];
				const end = next ? Math.min(ev.start + ev.duration, next.start) : ev.start + ev.duration;
				const left = Math.max(0, x(ev.start));
				const right = Math.min(width, x(end));
				if (right - left < 2)
					return;
				const running = ev.start <= t && t < end;
				/* a click on what runs now plays it (on the box itself: zaps
				 * to it), the right button or a long press opens the choices */
				const direct = running && (!onBox || may("write"));
				const timer = recordFor(ch, ev, end);
				const tip = (long) => `${hm(ev.start)}-${hm(end)} ${ev.title}${ev.description ? `\n${shortText(ev.description, 300)}` : ""}` +
					`${long ? `\n\n${shortText(long, 600)}` : ""}`;
				const el = h("div", { class: `event${running ? " running" : ""}${timer ? " timer" : ""}`, style: `left:${left}px;width:${right - left - 2}px`,
					title: tip(""),
					onclick: () => {
						if (!direct)
							eventDialog(ch, ev, el, series);
						else if (onBox)
							zap(ch).catch(fail);
						else
							stream(ch.id).catch(fail);
					},
					oncontextmenu: (e) => { e.preventDefault(); eventDialog(ch, ev, el, series); } }, ev.title);
				if (timer)
					el.timerId = timer.id;
				/* the long description comes on the first hover, the grid has only the short one */
				el.addEventListener("mouseenter", () => longText(ev).then((text) => {
					if (text)
						el.title = tip(text);
				}), { once: true });
				line.append(el);
			});
			/* the channel logo, the name where there is none */
			const head = h("div", { class: "rowh logo-head", title: ch.name });
			const logo = h("img", { src: `/api/v1/channels/${ch.id}/logo`, alt: ch.name, loading: "lazy" });
			logo.onerror = () => { head.classList.remove("logo-head"); head.replaceChildren(ch.name); };
			head.append(logo);
			grid_el.append(h("div", { class: "line" }, head, line));
		}
		grid_el.append(h("div", { class: "nowline", style: `left:${150 + x(t)}px` }));
		box.replaceChildren(grid_el);
	};
	const sel = bouquetSelect(bouquets, savedBouquet(bouquets), (id) => show(id).catch(fail));
	main.append(h("div", { class: "toolbar" }, h("h1", {}, tr("nav.epg")), sel), box);
	await show(sel.value);
}

/* the API names kinds like exec-plugin, the texts timer.kind.exec_plugin */
const timerKind = (kind) => {
	const name = `timer.kind.${String(kind).replace(/-/g, "_")}`;
	const text = tr(name);
	return text === name ? kind : text;
};

async function viewTimers(main) {
	const list = h("div", { class: "list" });
	const series = h("div", { class: "list" });
	main.append(h("div", { class: "toolbar" }, h("h1", {}, tr("nav.timers"))), list, series);
	let rules = [];
	/* the series timers below the timers: what they wait for, and the way to delete one */
	const showSeries = () => {
		if (!rules.length) {
			series.replaceChildren();
			return;
		}
		series.replaceChildren(h("h2", {}, tr("series.heading")), ...rules.map((r) => h("div", { class: "card timer" },
			h("div", {},
				h("div", {}, h("strong", {}, r.title)),
				h("div", { class: "dim small" },
					[r.channel, trn("series.waiting", r.timers), r.conflicts ? trn("series.conflicts", r.conflicts) : ""].filter(Boolean).join(" - "))),
			may("write") ? h("button", { class: "btn icon danger", title: tr("series.delete"), html: icon("trash"),
				onclick: async () => {
					if (!await confirmDialog(tr("series.delete_ask", { title: r.title }), tr("delete"), true))
						return;
					try {
						await api(`timers/series/${r.id}`, { method: "DELETE" });
						await show();
					} catch (e) { fail(e); }
				} }) : null)));
	};
	/* the timer list cuts long titles off */
	const ruleOf = (t) => t.kind !== "record" || !t.title ? undefined : rules.find((r) => r.channel_id === t.channel_id &&
		(sameSeries(r, t.channel_id, t.title) || (t.title.length >= 40 && seriesName(r.title).startsWith(t.title.trim().toLowerCase()))));
	/* all episodes of what a timer records, as "repeat: all episodes" in the
	   timer menu of the box; the title in full comes from the guide */
	const seriesButton = (t) => {
		const rule = ruleOf(t);
		return h("button", { class: `btn icon${rule ? " primary" : ""}`, html: icon("series"),
			title: rule ? tr("series.delete") : tr("series.record_all"),
			onclick: async () => {
				try {
					if (rule) {
						if (!await confirmDialog(tr("series.delete_ask", { title: rule.title }), tr("delete"), true))
							return;
						await api(`timers/series/${rule.id}`, { method: "DELETE" });
					} else {
						const ev = t.epg_id && t.epg_id !== "0"
							? await api(`epg/events/${t.epg_id}?start=${t.epg_start}`).catch(() => null) : null;
						await recordSeries(t.channel_id, { title: ev?.title || t.title });
					}
					await show();
				} catch (e) { fail(e); }
			} });
	};
	const show = async () => {
		let items;
		[items, rules] = await Promise.all([api("timers").then((r) => r.items || []), seriesRules()]);
		items.sort((a, b) => a.start - b.start);
		showSeries();
		if (!items.length) {
			list.replaceChildren(h("div", { class: "empty" }, tr("timers.none")));
			return;
		}
		const rows = [];
		for (const t of items) {
			const channel = t.channel_id && t.channel_id !== "0" ? await channelName(t.channel_id) : "";
			rows.push(h("div", { class: "card timer" },
				h("div", {},
					h("div", {}, h("strong", {}, t.title || timerKind(t.kind))),
					h("div", { class: "dim small" },
						`${timerKind(t.kind)} - ${day(t.start)} ${hm(t.start)}${t.stop ? `-${hm(t.stop)}` : ""}${channel ? ` - ${channel}` : ""}` +
						`${ruleOf(t) ? ` - ${tr("series.tag")}` : ""}`)),
				may("write") ? h("div", { class: "row" },
					t.kind === "record" && t.title ? seriesButton(t) : null,
					h("button", { class: "btn icon danger", title: tr("delete"), html: icon("trash"),
						onclick: async () => {
							if (!await confirmDialog(tr("timer.delete_ask", { title: t.title || timerKind(t.kind) }), tr("delete"), true))
								return;
							try {
								await api(`timers/${t.id}`, { method: "DELETE" });
								await show();
							} catch (e) { fail(e); }
						} })) : null));
		}
		list.replaceChildren(...rows);
	};
	await show();
}

function size(bytes) {
	const units = ["B", "KB", "MB", "GB", "TB"];
	let i = 0;
	while (bytes >= 1024 && i < units.length - 1) { bytes /= 1024; i++; }
	const digits = i ? 1 : 0;
	return `${new Intl.NumberFormat(language(), { minimumFractionDigits: digits, maximumFractionDigits: digits }).format(bytes)} ${units[i]}`;
}

function uptime(s) {
	const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), mm = Math.floor(s % 3600 / 60);
	return d ? tr("uptime.days", { d, h: hh }) : tr("uptime.hours", { h: hh, m: mm });
}

async function viewBox(main) {
	const [info, standby] = await Promise.all([api("system/info"), api("system/standby").catch(() => null)]);
	const cards = h("div", { class: "cards" });
	main.append(h("h1", {}, tr("nav.box")), cards);

	cards.append(h("div", { class: "card" }, h("h2", {}, info.hostname || tr("nav.box")),
		h("dl", { class: "facts" },
			h("dt", {}, tr("box.model")), h("dd", {}, `${info.vendor} ${info.model}`.trim()),
			h("dt", {}, tr("box.image")), h("dd", {}, info.image_version || "-"),
			h("dt", {}, tr("box.kernel")), h("dd", {}, info.kernel || "-"),
			h("dt", {}, tr("box.running")), h("dd", {}, uptime(info.uptime || 0)),
			h("dt", {}, tr("box.memory")), h("dd", {}, tr("free_of", { free: size(info.memory_free), total: size(info.memory_total) })),
			h("dt", {}, tr("box.storage")), h("dd", {}, tr("free_of", { free: size(info.root_free), total: size(info.root_total) })))));

	if (may("write")) {
		const text = h("textarea", { maxlength: 1024, placeholder: tr("box.message_placeholder"), "aria-label": tr("box.message") });
		const popup = h("input", { type: "checkbox" });
		cards.append(h("div", { class: "card" }, h("h2", {}, tr("box.message")),
			h("form", { onsubmit: async (ev) => {
				ev.preventDefault();
				if (!text.value.trim())
					return;
				try {
					await api("system/message", { method: "POST", body: { text: text.value, popup: popup.checked } });
					text.value = "";
					toast(tr("box.message_sent"));
				} catch (e) { fail(e); }
			} }, text,
				h("div", { class: "row", style: "margin-top:10px" },
					h("label", { class: "check" }, popup, tr("box.wait_ok")),
					h("span", { style: "flex:1" }),
					h("button", { class: "btn primary" }, tr("send"))))));
	}

	if (may("system")) {
		const power = async (action, question) => {
			if (!await confirmDialog(question, tr("yes"), true))
				return;
			try {
				await api("system/power", { method: "POST", body: { action } });
				toast(tr("done"));
			} catch (e) { fail(e); }
		};
		let asleep = !!standby?.on;
		const sb = h("button", { class: "btn", onclick: async () => {
			try {
				await api("system/standby", { method: "POST", body: { on: !asleep } });
				asleep = !asleep;
				sb.textContent = asleep ? tr("box.wake") : tr("box.standby");
			} catch (e) { fail(e); }
		} }, asleep ? tr("box.wake") : tr("box.standby"));
		cards.append(h("div", { class: "card" }, h("h2", {}, tr("box.power")),
			h("div", { class: "row" }, sb,
				h("button", { class: "btn", onclick: () => power("restart", tr("box.restart_ask")) }, tr("box.restart")),
				h("button", { class: "btn danger", onclick: () => power("reboot", tr("box.reboot_ask")) }, tr("box.reboot")),
				h("button", { class: "btn danger", onclick: () => power("shutdown", tr("box.shutdown_ask")) }, tr("box.shutdown")))));
	}

	const s = await session();
	const account = h("div", { class: "card" }, h("h2", {}, tr("browser.title")),
		h("p", { class: "dim small" }, s.authenticated ? tr("browser.access_user", { level: tr(`level.${s.level}`), user: s.user })
			: s.device ? tr("browser.access_device", { level: tr(`level.${s.level}`), device: s.device })
			: tr("browser.access_home", { level: tr(`level.${s.level}`) })));
	const buttons = h("div", { class: "row" });
	if (s.authenticated)
		buttons.append(h("button", { class: "btn", onclick: async () => {
			try { await api("logout", { method: "POST" }); } catch { /* gone anyway */ }
			location.hash = "#/signin";
		} }, tr("sign_out")));
	else if (s.device)
		buttons.append(h("button", { class: "btn", onclick: async () => {
			if (!await confirmDialog(tr("browser.unpair_ask"), tr("browser.unpair"), true))
				return;
			try { await api("pair/forget", { method: "POST" }); } catch (e) { fail(e); return; }
			await session();
			location.hash = "#/signin";
		} }, tr("browser.unpair")));
	else if (s.secure)
		buttons.append(h("a", { class: "btn", href: "#/signin" }, tr("pair.title")));
	else if (httpsUrl())
		buttons.append(h("a", { class: "btn", href: httpsUrl("/#/signin") }, tr("https.switch")));
	buttons.append(h("a", { class: "btn", href: "/api/v1/system/ca", title: tr("browser.ca_hint") }, tr("browser.ca")));
	account.append(buttons);
	cards.append(account);

	if (may("system")) {
		cards.append(await devicesCard());
		const mcp = await api("mcp").catch(() => null);
		if (mcp?.available)
			cards.append(mcpCard(mcp));
	}
}

/* the MCP server of the assistant, for claude.ai and other MCP clients */
function mcpCard(state) {
	const card = h("div", { class: "card" });
	const show = (st) => {
		const enabled = h("input", { type: "checkbox", checked: st.enabled });
		enabled.addEventListener("change", () => change({ enabled: enabled.checked }));
		const url = h("input", { type: "text", value: st.public_url, placeholder: "https://...", "aria-label": tr("mcp.public_address"), style: "flex:1;min-width:0" });
		const fresh = st.new_token ? h("div", { class: "row" },
			h("code", { style: "overflow-wrap:anywhere" }, st.new_token),
			h("button", { class: "btn", onclick: async () => {
				try { await navigator.clipboard.writeText(st.new_token); toast(tr("mcp.token_copied")); } catch { /* shown above */ }
			} }, tr("copy"))) : null;
		const now = Date.now() / 1000;
		const clients = (st.clients || []).filter((c) => c.signed_in_until > now);
		card.replaceChildren(h("div", {}, h("h2", {}, tr("mcp.title")),
			h("label", { class: "check" }, enabled, tr("mcp.open")),
			h("dl", { class: "facts", style: "margin-top:10px" },
				h("dt", {}, tr("mcp.address")), h("dd", {}, st.address || "-"),
				h("dt", {}, tr("mcp.token")), h("dd", {}, st.token || "-")),
			fresh ? h("p", { class: "small" }, tr("mcp.new_token_shown")) : null, fresh,
			h("div", { class: "row", style: "margin-top:10px" },
				h("button", { class: "btn", onclick: async () => {
					if (await confirmDialog(tr("mcp.new_token_ask"), tr("mcp.new_token"), true))
						change({ new_token: true });
				} }, tr("mcp.new_token"))),
			h("p", { class: "dim small" }, tr("mcp.public_hint")),
			h("div", { class: "row" }, url, h("button", { class: "btn", onclick: () => change({ public_url: url.value.trim() }) }, tr("save"))),
			h("h2", { style: "margin-top:16px" }, tr("mcp.clients")),
			clients.length ? h("div", { class: "list" }, clients.map((c) => h("div", { class: "timer" },
				h("div", {}, h("div", {}, c.name || c.id), h("div", { class: "dim small" }, tr("mcp.client_until", { host: c.host, day: day(c.signed_in_until) }))),
				h("button", { class: "btn", onclick: async () => {
					if (!await confirmDialog(tr("mcp.sign_out_ask", { name: c.name || c.id }), tr("sign_out"), true))
						return;
					try {
						await api(`mcp/clients/${encodeURIComponent(c.id)}`, { method: "DELETE" });
						show(await api("mcp"));
					} catch (e) { fail(e); }
				} }, tr("sign_out"))))) : h("p", { class: "dim small" }, tr("mcp.no_clients")),
			clients.length > 1 ? h("button", { class: "btn danger", style: "margin-top:8px", onclick: async () => {
				if (!await confirmDialog(tr("mcp.sign_out_all_ask"), tr("mcp.sign_out_all"), true))
					return;
				try {
					await api("mcp/clients", { method: "DELETE" });
					show(await api("mcp"));
				} catch (e) { fail(e); }
			} }, tr("mcp.sign_out_all")) : null));
	};
	const change = async (body) => {
		try {
			show(await api("mcp", { method: "POST", body }));
			toast(tr("saved"));
		} catch (e) { fail(e); }
	};
	show(state);
	return card;
}

async function devicesCard() {
	const card = h("div", { class: "card" }, h("h2", {}, tr("devices.title")));
	const list = h("div", { class: "list" });
	card.append(list);
	const show = async () => {
		const items = (await api("pair/devices")).items || [];
		if (!items.length) {
			list.replaceChildren(h("p", { class: "dim small" }, tr("devices.none")));
			return;
		}
		list.replaceChildren(...items.sort((a, b) => b.last_used - a.last_used).map((d) =>
			h("div", { class: "timer" },
				h("div", {}, h("div", {}, d.name), h("div", { class: "dim small" }, tr("devices.last_used", { day: day(d.last_used), time: hm(d.last_used) }))),
				h("button", { class: "btn icon danger", title: tr("remove"), html: icon("trash"), onclick: async () => {
					if (!await confirmDialog(tr("devices.remove_ask", { name: d.name }), tr("remove"), true))
						return;
					try {
						await api(`pair/devices/${d.id}`, { method: "DELETE" });
						await show();
					} catch (e) { fail(e); }
				} }))));
	};
	await show();
	return card;
}

function pairCard(s) {
	const card = h("div", { class: "card signin" }, h("h1", {}, tr("pair.title")));
	if (!s.secure) {
		const url = httpsUrl("/#/signin");
		card.append(h("p", { class: "dim" }, trx("pair.https", { link: h("a", { href: "/api/v1/system/ca" }, tr("pair.install_ca")) })),
			url ? h("a", { class: "btn primary", href: url }, tr("https.switch")) : h("p", { class: "error small" }, tr("https.off")));
		return card;
	}
	const out = h("p", { class: "dim" }, tr("pair.how"));
	const ask = h("button", { class: "btn primary", onclick: async () => {
		ask.disabled = true;
		try {
			await pairOnTv((digits) => { out.textContent = tr("pair.allow", { code: digits }); });
			location.hash = "#/remote";
		} catch (e) {
			out.textContent = e.message || tr("pair.refused");
			ask.disabled = false;
		}
	} }, tr("pair.ask"));
	card.append(out, ask);
	return card;
}

async function viewSignIn(main) {
	const s = await session().catch(() => ({ level: "public" }));
	main.append(pairCard(s));
	const user = h("input", { type: "text", autocomplete: "username", value: "root", "aria-label": tr("signin.name") });
	const pass = h("input", { type: "password", autocomplete: "current-password", "aria-label": tr("signin.password"), placeholder: tr("signin.password") });
	const msg = h("p", { class: "error small" });
	main.append(h("div", { class: "card signin" },
		h("h1", {}, tr("signin.title")),
		s.level !== "public" ? h("p", { class: "dim" }, trx("signin.has_access", { level: tr(`level.${s.level}`), link: h("a", { href: "#/remote" }, tr("continue")) })) : null,
		h("form", { onsubmit: async (ev) => {
			ev.preventDefault();
			msg.textContent = "";
			try {
				await api("login", { method: "POST", body: { user: user.value, password: pass.value } });
				await session();
				location.hash = "#/remote";
			} catch (e) {
				msg.textContent = e.code === "authentication-off"
					? tr("signin.off")
					: e.message;
			}
		} }, user, pass, h("button", { class: "btn primary" }, tr("signin.button")), msg)));
}

/* ------------------------------------------------------------------ */
/* recordings                                                          */

function renameDialog(title) {
	return new Promise((resolve) => {
		const input = h("input", { type: "text", required: true, value: title, maxlength: 200, "aria-label": tr("title") });
		const d = h("dialog", {}, h("form", { method: "dialog", class: "fields" },
			h("h2", {}, tr("recordings.rename_title")),
			h("label", { class: "field" }, h("span", { class: "dim small" }, tr("title")), input),
			h("div", { class: "buttons" },
				h("button", { class: "btn", type: "button", onclick: () => d.close() }, tr("cancel")),
				h("button", { class: "btn primary", value: "ok" }, tr("rename")))));
		d.addEventListener("close", () => {
			d.remove();
			resolve(d.returnValue === "ok" && input.value.trim() ? input.value.trim() : null);
		});
		document.body.append(d);
		d.showModal();
		input.select();
	});
}

function savedRecordingsView() {
	try { return localStorage.getItem("recordings_view") === "wall" ? "wall" : "list"; } catch { return "list"; }
}

function saveRecordingsView(view) {
	try { localStorage.setItem("recordings_view", view); } catch { /* no storage */ }
}

const coverUrl = (r) => `/api/v1/recordings/${r.id}/cover?v=${r.cover_time || 0}`;

/* the picture of a recording, upright as a poster: a wide one fitted in,
   over a blurred copy of itself */
function tilePicture(r) {
	if (!r?.cover)
		return h("div", { class: "pic none", html: icon("recordings") });
	const img = h("img", { src: coverUrl(r), alt: "", loading: "lazy" });
	if (!r.cover_wide)
		return h("div", { class: "pic" }, img);
	return h("div", { class: "pic wide", style: `--bg: url("${coverUrl(r)}")` }, img);
}

const episodes = (n) => trn("recordings.episodes", n);

/* a row of tiles that scrolls sideways, with < and > where there is more */
function shelfStrip(tiles) {
	const row = h("div", { class: "tiles" }, ...tiles);
	const step = (dir) => () => row.scrollBy({ left: dir * row.clientWidth * 0.9, behavior: "smooth" });
	const prev = h("button", { class: "nav prev", title: tr("back"), "aria-label": tr("back"), onclick: step(-1) }, "‹");
	const next = h("button", { class: "nav next", title: tr("more"), "aria-label": tr("more"), onclick: step(1) }, "›");
	const update = () => {
		prev.hidden = row.scrollLeft <= 1;
		next.hidden = row.scrollLeft + row.clientWidth >= row.scrollWidth - 1;
	};
	row.addEventListener("scroll", update, { passive: true });
	new ResizeObserver(update).observe(row);
	return h("div", { class: "strip" }, prev, row, next);
}

/* what tells one episode from another: "Tatort: Ein Freund, ein guter Freund"
   gives its part after the series, "S17E4 ║ Ein letzter Tropfen ║ 2015 ║ 12+ ║ GB"
   the number and the first part that is no year, age or country and does not
   just name the series again */
function episodeName(r) {
	const plain = (s) => s.toLowerCase().replace(/[^\p{L}\p{N}]/gu, "");
	const series = r.serie || "";
	let name = "";
	if (series && r.title.length > series.length && r.title.startsWith(series))
		name = r.title.slice(series.length).replace(/^\s*[:\-–]\s*/, "");
	let number = "";
	for (const part of (r.info || "").split(/\s*[║|]\s*/).map((s) => s.trim()).filter(Boolean)) {
		if (/^S\d+\s*E\d+$/i.test(part) || /^\(\d+\)$/.test(part)) {
			number ||= part.replace(/[()]/g, "");
			continue;
		}
		if (/^\d{4}$/.test(part) || /^\d+\+$/.test(part) || /^[A-Z]{1,3}$/.test(part))
			continue;
		if (!name && !plain(part).includes(plain(r.title)) && !(series && plain(part).includes(plain(series))))
			name = part;
	}
	return [/^\d+$/.test(number) ? `#${number}` : number, name].filter(Boolean).join(" · ");
}

async function viewRecordings(main) {
	let view = savedRecordingsView();
	let items = [];
	let shown = "";
	const content = h("div", {});
	const run = async (fn, done) => {
		try {
			await fn();
			if (done)
				toast(done);
			await show(true);
		} catch (e) { fail(e); }
	};
	/* what can be done with a recording, in the list and in a tile's dialog */
	const actions = (r, closing) => {
		const buttons = h("div", { class: "row" });
		const then = (fn) => async () => { closing?.(); await fn(); };
		if (!onBox)
			buttons.append(h("button", { class: "btn icon", title: tr("recordings.play_here"), html: icon("stream"),
				onclick: () => playUrl(`${location.origin}/api/v1/recordings/${r.id}/file`).catch(fail) }));
		if (may("write"))
			buttons.append(h("button", { class: "btn", onclick: then(async () => {
				try {
					await api(`recordings/${r.id}/play`, { method: "POST" });
					toast(tr("recordings.playing_tv", { title: r.title }));
				} catch (e) { fail(e); }
			}) }, tr("recordings.play_tv")));
		if (may("system") && !r.busy) {
			buttons.append(h("button", { class: "btn", onclick: then(async () => {
				const title = await renameDialog(r.title);
				if (title && title !== r.title)
					await run(() => api(`recordings/${r.id}/rename`, { method: "POST", body: { title } }), tr("recordings.renamed"));
			}) }, tr("rename")));
			buttons.append(h("button", { class: "btn icon danger", title: tr("delete"), html: icon("trash"), onclick: then(async () => {
				if (await confirmDialog(tr("recordings.delete_ask", { title: r.title }), tr("delete"), true))
					await run(() => api(`recordings/${r.id}`, { method: "DELETE" }), tr("recordings.deleted"));
			}) }));
		}
		return buttons;
	};
	const meta = (r) => [r.genre_detail || r.genre, r.channel, `${day(r.time)} ${hm(r.time)}`, r.length ? tr("minutes", { n: r.length }) : "",
		size(r.size), r.busy ? tr("recordings.now") : ""].filter(Boolean).join(" · ");
	const row = (r) => {
		return h("div", { class: "card recording" }, tilePicture(r),
			h("div", {},
				h("strong", {}, r.title),
				h("div", { class: "dim small" }, meta(r)),
				r.info ? h("div", { class: "dim small" }, r.info) : null),
			actions(r));
	};
	const details = (r) => {
		const d = h("dialog", { class: "recording-details" });
		const close = () => d.close();
		d.append(...[r.cover ? h("img", { class: r.cover_wide ? "wide" : "", src: coverUrl(r), alt: "" }) : null,
			h("h2", {}, r.title),
			episodeName(r) && !r.title.includes(episodeName(r)) ? h("div", {}, episodeName(r)) : null,
			h("div", { class: "dim small" }, meta(r)),
			r.info ? h("p", { class: "small" }, r.info) : null,
			actions(r, close),
			h("div", { class: "buttons" }, h("button", { class: "btn", onclick: close }, tr("close")))].filter(Boolean));
		d.addEventListener("close", () => d.remove());
		document.body.append(d);
		d.showModal();
	};
	const tile = (r) => h("button", { class: "tile", title: r.title, onclick: () => details(r) },
		tilePicture(r),
		r.busy ? h("span", { class: "badge live" }, "REC") : null,
		h("span", { class: "caption" }, r.serie || r.title),
		h("span", { class: "dim small" }, episodeName(r) || `${day(r.time)} ${hm(r.time)}`));
	/* a series: named so (by TMDB or by hand), or numbered as an episode
	   by the EPG; DVB has no other mark for one */
	const seriesOf = (r) => r.serie || (r.episode ? r.title : "");
	const parts = (n) => trn("recordings.parts", n);
	const episodeList = (name, list, count = episodes) => {
		const d = h("dialog", { class: "recording-details episodes" });
		const close = () => d.close();
		const pick = (r) => h("div", { class: "card recording" }, tilePicture(r),
			h("div", {}, h("strong", {}, episodeName(r) || r.title), h("div", { class: "dim small" }, meta(r))),
			actions(r, close));
		d.append(h("h2", {}, name), h("div", { class: "dim small" }, count(list.length)),
			h("div", { class: "list" }, ...list.map(pick)),
			h("div", { class: "buttons" }, h("button", { class: "btn", onclick: close }, tr("close"))));
		d.addEventListener("close", () => d.remove());
		document.body.append(d);
		d.showModal();
	};
	const seriesTile = (name, list, count = episodes) => {
		const pic = list.find((r) => r.cover && !r.cover_wide) || list.find((r) => r.cover);
		return h("button", { class: "tile", title: name, onclick: () => episodeList(name, list, count) },
			tilePicture(pic),
			list.some((r) => r.busy) ? h("span", { class: "badge live" }, "REC") : null,
			h("span", { class: "caption" }, name),
			h("span", { class: "dim small" }, count(list.length)));
	};
	/* one tile per title: the episodes of a series, or the parts and
	   repeats of anything else, in the shelf where it belongs */
	const titleTiles = (list) => {
		const byTitle = new Map();
		for (const r of list) {
			const key = seriesOf(r) || r.title;
			if (!byTitle.has(key))
				byTitle.set(key, []);
			byTitle.get(key).push(r);
		}
		return [...byTitle.entries()].map(([name, rs]) =>
			rs.length === 1 ? tile(rs[0]) : seriesTile(name, rs, seriesOf(rs[0]) ? episodes : parts));
	};
	/* shelves as in the media libraries: the newest, films, series,
	   documentaries (a series of them as one tile there), then one per genre,
	   the fullest first, and what has no genre at the end */
	const wall = () => {
		const shelf = (title, tiles) => h("section", { class: "shelf" }, h("h2", {}, title), shelfStrip(tiles));
		const movies = [], series = new Map(), docs = [], groups = new Map();
		for (const r of items) {
			const name = seriesOf(r);
			if (r.documentary)
				docs.push(r);
			else if (name) {
				if (!series.has(name))
					series.set(name, []);
				series.get(name).push(r);
			} else if (r.genre_class === 1)
				movies.push(r);
			else {
				const key = r.genre || "";
				if (!groups.has(key))
					groups.set(key, []);
				groups.get(key).push(r);
			}
		}
		const named = [...groups.entries()].filter(([k]) => k).sort((a, b) => b[1].length - a[1].length);
		const shelves = [shelf(tr("wall.newest"), items.slice(0, 12).map(tile))];
		if (movies.length)
			shelves.push(shelf(tr("wall.movies"), titleTiles(movies)));
		if (series.size)
			shelves.push(shelf(tr("wall.series"), [...series.entries()].map(([name, list]) => seriesTile(name, list))));
		if (docs.length)
			shelves.push(shelf(tr("wall.documentaries"), titleTiles(docs)));
		shelves.push(...named.map(([k, list]) => shelf(k, titleTiles(list))));
		if (groups.has(""))
			shelves.push(shelf(tr("wall.other"), titleTiles(groups.get(""))));
		return h("div", { class: "wall" }, ...shelves);
	};
	const render = () => {
		if (!items.length)
			content.replaceChildren(h("div", { class: "card empty" }, tr("recordings.none")));
		else
			content.replaceChildren(view === "wall" ? wall() : h("div", { class: "list" }, ...items.map(row)));
	};
	/* a refresh that changed nothing keeps the shelves where they were scrolled */
	const show = async (force) => {
		items = ((await api("recordings")).items || []).sort((a, b) => b.time - a.time);
		const now = JSON.stringify(items);
		if (force || now !== shown) {
			shown = now;
			render();
		}
	};
	const toggle = (to, label) => h("button", { class: "btn", "aria-pressed": String(view === to), onclick: (ev) => {
		view = to;
		saveRecordingsView(view);
		for (const b of ev.currentTarget.parentNode.children)
			b.setAttribute("aria-pressed", String(b === ev.currentTarget));
		render();
	} }, label);
	main.append(h("div", { class: "toolbar" }, h("h1", {}, tr("nav.recordings")),
		h("div", { class: "segmented", role: "group", "aria-label": tr("recordings.view") }, toggle("list", tr("recordings.list")), toggle("wall", tr("recordings.wall"))),
		may("system") ? h("button", { class: "btn", title: tr("recordings.fetch_hint"), onclick: async () => {
			try {
				const r = await api("recordings/covers", { method: "POST" });
				toast(r?.queued ? trn("recordings.fetching", r.queued) : tr("recordings.all_have_pictures"));
			} catch (e) { fail(e); }
		} }, tr("recordings.fetch")) : null,
		h("button", { class: "btn icon", title: tr("reload"), html: icon("refresh"), onclick: () => show(true).catch(fail) })), content);
	await show(true);

	/* covers arrive in the background, recordings end */
	const refresh = () => { if (!document.hidden) show().catch(() => {}); };
	const timer = setInterval(refresh, 10000);
	document.addEventListener("visibilitychange", refresh);
	cleanup = () => {
		clearInterval(timer);
		document.removeEventListener("visibilitychange", refresh);
	};
}

/* ------------------------------------------------------------------ */
/* drives and network shares                                           */

const SHARE_TYPES = { smb: "SMB", nfs: "NFS", ftp: "FTP" };

function usage(d) {
	if (!d.size)
		return null;
	const used = Math.round(100 * (d.size - d.free) / d.size);
	return h("div", {},
		h("div", { class: "progress" }, h("span", { style: `width:${used}%` })),
		h("div", { class: "dim small" }, tr("free_of", { free: size(d.free), total: size(d.size) })));
}

function holds(dir, path) {
	if (!dir || !path)
		return false;
	return path === dir || path.startsWith(dir.endsWith("/") ? dir : `${dir}/`);
}

function shareLabel(s) {
	return s.type === "nfs" ? `${s.host}:${s.share}` : `//${s.host}/${s.share}`;
}

function localDirFor(share) {
	return `/media/net/${share.replace(/^\/+/, "").replace(/[^\w.-]+/g, "_") || "share"}`;
}

/* asks for what a share entry holds; resolves to the body, or null on cancel */
function shareDialog(title, init, isNew) {
	return new Promise((resolve) => {
		const type = h("select", { "aria-label": tr("mounts.type") },
			h("option", { value: "smb" }, tr("mounts.type_smb")),
			h("option", { value: "nfs" }, tr("mounts.type_nfs")));
		type.value = init.type || "smb";
		const field = (label, input) => h("label", { class: "field" }, h("span", { class: "dim small" }, label), input);
		const shareName = h("span", { class: "dim small" });
		const host = h("input", { type: "text", required: true, value: init.host || "", placeholder: tr("mounts.host_placeholder") });
		const share = h("input", { type: "text", required: true, value: init.share || "" });
		const dir = h("input", { type: "text", required: true, value: init.local_dir || "" });
		const user = h("input", { type: "text", value: init.username || "", autocomplete: "off" });
		const pass = h("input", { type: "password", value: init.password || "", autocomplete: "new-password",
			placeholder: init.has_password ? tr("mounts.unchanged") : "" });
		const options = h("input", { type: "text", value: init.options || "", placeholder: "rw" });
		const auto = h("input", { type: "checkbox" });
		auto.checked = init.automount !== false;
		const record = h("input", { type: "checkbox" });
		const login = h("div", { class: "fields" }, field(tr("mounts.user"), user), field(tr("signin.password"), pass));
		const sync = () => {
			login.hidden = type.value === "nfs";
			share.placeholder = type.value === "nfs" ? "/export/path" : tr("mounts.share_placeholder");
			shareName.textContent = type.value === "nfs" ? tr("mounts.export") : tr("mounts.share");
		};
		type.addEventListener("change", sync);
		sync();
		if (!init.local_dir)
			share.addEventListener("input", () => { dir.value = localDirFor(share.value); });

		const form = h("form", { method: "dialog", class: "fields" },
			h("h2", {}, title),
			field(tr("mounts.type"), type), field(tr("mounts.server"), host), h("label", { class: "field" }, shareName, share),
			login, field(tr("mounts.mount_at"), dir), field(tr("mounts.options"), options),
			h("label", { class: "check" }, auto, tr("mounts.automount")),
			isNew ? h("label", { class: "check" }, record, tr("mounts.record_here")) : null,
			h("div", { class: "buttons" },
				h("button", { class: "btn", type: "button", onclick: () => d.close() }, tr("cancel")),
				h("button", { class: "btn primary", value: "ok" }, isNew ? tr("mounts.add_mount") : tr("save"))));
		const d = h("dialog", {}, form);
		d.addEventListener("close", () => {
			d.remove();
			if (d.returnValue !== "ok")
				return resolve(null);
			const body = { type: type.value, host: host.value.trim(), share: share.value.trim(),
				local_dir: dir.value.trim(), automount: auto.checked, options: options.value.trim() };
			if (type.value === "smb") {
				body.username = user.value.trim();
				if (pass.value || !init.has_password || !body.username)
					body.password = pass.value;
			}
			resolve({ body, record: record.checked });
		});
		document.body.append(d);
		d.showModal();
	});
}

function loginDialog(host) {
	return new Promise((resolve) => {
		const user = h("input", { type: "text", required: true, autocomplete: "off", "aria-label": tr("mounts.user") });
		const pass = h("input", { type: "password", autocomplete: "new-password", "aria-label": tr("signin.password") });
		const d = h("dialog", {}, h("form", { method: "dialog", class: "fields" },
			h("h2", {}, tr("mounts.sign_in_to", { host })),
			h("label", { class: "field" }, h("span", { class: "dim small" }, tr("mounts.user")), user),
			h("label", { class: "field" }, h("span", { class: "dim small" }, tr("signin.password")), pass),
			h("div", { class: "buttons" },
				h("button", { class: "btn", type: "button", onclick: () => d.close() }, tr("cancel")),
				h("button", { class: "btn primary", value: "ok" }, tr("mounts.list_shares")))));
		d.addEventListener("close", () => {
			d.remove();
			resolve(d.returnValue === "ok" ? { username: user.value.trim(), password: pass.value } : null);
		});
		document.body.append(d);
		d.showModal();
	});
}

async function viewMounts(main) {
	const drives = h("div", { class: "cards" });
	const shares = h("div", { class: "list" });
	const found = h("div", { class: "list" });
	let state = null;

	const run = async (btn, fn, done) => {
		if (btn)
			btn.disabled = true;
		try {
			const result = await fn();
			if (done)
				toast(typeof done === "function" ? done(result) : done);
			await show();
		} catch (e) {
			fail(e);
		} finally {
			if (btn)
				btn.disabled = false;
		}
	};
	const recordHere = (path) => api("mounts/record", { method: "POST", body: { path } });
	const recordButton = (dir, target, writable) => {
		if (!may("system") || !dir || !writable || holds(dir, state.record_dir))
			return null;
		return h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
			() => recordHere(target), tr("mounts.recordings_here")) }, tr("mounts.record_here"));
	};
	/* unmounting the record target moves the recordings; the box says where */
	const unmounted = (what) => (r) => r?.record_dir ? tr(`mounts.${what}_unmounted_moved`, { dir: r.record_dir }) : tr(`mounts.${what}_unmounted`);
	const marker = (dir) => holds(dir, state.record_dir)
		? h("span", { class: "badge", title: state.record_dir }, h("span", { html: icon("record") }), tr("nav.recordings")) : null;

	const addShare = async (init) => {
		const r = await shareDialog(tr("mounts.add_share"), init, true);
		if (!r)
			return;
		await run(null, async () => {
			const s = await api("mounts/shares", { method: "POST", body: r.body });
			try {
				await api(`mounts/shares/${s.nr}/mount`, { method: "POST" });
			} catch (e) {
				/* an entry that never mounted is not kept */
				await api(`mounts/shares/${s.nr}`, { method: "DELETE" }).catch(() => {});
				throw e;
			}
			if (r.record)
				await recordHere(s.local_dir);
		}, tr("mounts.share_mounted"));
	};

	const renderDrives = () => {
		if (!state.drives.length) {
			drives.replaceChildren(h("div", { class: "card empty" }, tr("mounts.no_drives")));
			return;
		}
		drives.replaceChildren(...state.drives.map((d) => {
			const buttons = h("div", { class: "row", style: "margin-top:10px" });
			if (state.can_mount && may("write"))
				buttons.append(d.mounted
					? h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/drives/${d.name}/umount`, { method: "POST" }), unmounted("drive")) }, tr("mounts.unmount"))
					: h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/drives/${d.name}/mount`, { method: "POST" }), tr("mounts.drive_mounted")) }, tr("mounts.mount")));
			if (d.mounted && d.mount_point) {
				const b = recordButton(d.mount_point, `${d.mount_point}/movies`, d.writable);
				if (b)
					buttons.append(b);
			}
			return h("div", { class: "card" },
				h("div", { class: "row" }, h("h2", { style: "margin:0;flex:1" }, d.label || d.name), marker(d.mount_point)),
				h("div", { class: "dim small" }, [d.name, d.fs, d.mounted ? d.mount_point || tr("mounts.mounted_lower") : tr("mounts.not_mounted_lower")].filter(Boolean).join(" · ")),
				usage(d), buttons.childElementCount ? buttons : null);
		}));
	};

	const renderShares = () => {
		if (!state.shares.length) {
			shares.replaceChildren(h("div", { class: "cards" },
				h("div", { class: "card empty" }, state.can_mount ? tr("mounts.no_shares") : tr("mounts.no_shares_mounted"))));
			return;
		}
		shares.replaceChildren(...state.shares.map((s) => {
			const buttons = h("div", { class: "row" });
			if (!s.managed) {
				buttons.append(recordButton(s.local_dir, s.local_dir, s.writable) || "");
				return h("div", { class: "card share" },
					h("div", {},
						h("div", { class: "row" }, h("strong", {}, shareLabel(s)),
							h("span", { class: "badge on" }, tr("mounts.by_desktop")), marker(s.local_dir)),
						h("div", { class: "dim small" }, `${SHARE_TYPES[s.type] || s.type} · ${s.local_dir}`),
						usage(s)),
					buttons);
			}
			if (may("write"))
				buttons.append(s.mounted
					? h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/shares/${s.nr}/umount`, { method: "POST" }), unmounted("share")) }, tr("mounts.unmount"))
					: h("button", { class: "btn", onclick: (ev) => run(ev.currentTarget,
						() => api(`mounts/shares/${s.nr}/mount`, { method: "POST" }), tr("mounts.share_mounted")) }, tr("mounts.mount")));
			if (may("system")) {
				if (s.mounted)
					buttons.append(recordButton(s.local_dir, s.local_dir, s.writable) || "");
				if (!s.mounted)
					buttons.append(h("button", { class: "btn", onclick: async () => {
						const r = await shareDialog(tr("mounts.change_share"), s, false);
						if (r)
							await run(null, () => api(`mounts/shares/${s.nr}`, { method: "POST", body: r.body }), tr("saved"));
					} }, tr("edit")));
				buttons.append(h("button", { class: "btn icon danger", title: tr("remove"), html: icon("trash"),
					onclick: async (ev) => {
						if (await confirmDialog(tr("mounts.remove_ask", { share: shareLabel(s) }), tr("remove"), true))
							await run(ev.currentTarget, () => api(`mounts/shares/${s.nr}`, { method: "DELETE" }),
								(r) => r?.record_dir ? tr("mounts.share_removed_moved", { dir: r.record_dir }) : tr("mounts.share_removed"));
					} }));
			}
			return h("div", { class: "card share" },
				h("div", {},
					h("div", { class: "row" }, h("strong", {}, shareLabel(s)),
						h("span", { class: `badge ${s.mounted ? "on" : ""}` }, s.mounted ? tr("mounts.mounted") : tr("mounts.not_mounted")),
						h("span", { class: "badge" }, s.automount ? tr("mounts.at_boot") : tr("mounts.manual")),
						marker(s.local_dir)),
					h("div", { class: "dim small" }, `${SHARE_TYPES[s.type] || s.type} · ${s.local_dir}`),
					s.mounted ? usage(s) : null),
				buttons);
		}));
	};

	const show = async () => {
		state = await api("mounts");
		renderDrives();
		renderShares();
	};

	const browse = async (host, type, row) => {
		const label = host.name || host.ip;
		const out = h("div", { class: "list" }, h("div", { class: "dim small" }, tr("mounts.asking")));
		row.querySelector(".shares")?.remove();
		out.classList.add("shares");
		row.append(out);
		let creds = {};
		for (;;) {
			try {
				const items = (await api("mounts/browse", { method: "POST", body: { host: host.ip, type, ...creds } })).items || [];
				if (!items.length) {
					out.replaceChildren(h("div", { class: "dim small" }, tr("mounts.no_list")));
					return;
				}
				out.replaceChildren(...items.map((it) => h("div", { class: "row" },
					h("span", { style: "flex:1" }, h("strong", {}, it.name), it.comment ? h("span", { class: "dim small" }, ` ${it.comment}`) : null),
					h("button", { class: "btn", onclick: () => addShare({ type, host: host.ip, share: it.name,
						local_dir: localDirFor(it.name), ...creds }) }, tr("add")))));
				return;
			} catch (e) {
				if (e instanceof ApiError && e.code === "login-needed") {
					const c = await loginDialog(label);
					if (!c) {
						out.remove();
						return;
					}
					creds = c;
					continue;
				}
				if (e instanceof ApiError && e.code === "no-share-list") {
					out.replaceChildren(h("div", { class: "row" },
						h("span", { class: "dim small", style: "flex:1" }, e.message),
						h("button", { class: "btn", onclick: () => addShare({ type, host: host.ip }) }, tr("mounts.add_by_name"))));
					return;
				}
				out.remove();
				fail(e);
				return;
			}
		}
	};

	const search = async (btn) => {
		btn.disabled = true;
		found.replaceChildren(h("div", { class: "empty" }, tr("mounts.searching")));
		try {
			const hosts = (await api("mounts/scan", { method: "POST" })).items || [];
			if (!hosts.length) {
				found.replaceChildren(h("div", { class: "empty" }, tr("mounts.no_servers")));
				return;
			}
			found.replaceChildren(h("h2", { style: "margin-top:20px" }, tr("mounts.servers")), ...hosts.map((host) => {
				const row = h("div", { class: "card" });
				row.append(h("div", { class: "row" },
					h("span", { style: "flex:1" }, h("strong", {}, host.name || host.ip), host.name ? h("span", { class: "dim small" }, ` ${host.ip}`) : null),
					host.smb ? h("button", { class: "btn", onclick: () => browse(host, "smb", row) }, tr("mounts.smb_shares")) : null,
					host.nfs ? h("button", { class: "btn", onclick: () => browse(host, "nfs", row) }, tr("mounts.nfs_exports")) : null));
				return row;
			}));
		} catch (e) {
			found.replaceChildren();
			fail(e);
		} finally {
			btn.disabled = false;
		}
	};

	await show();
	/* a Neutrino without root (a desktop) shows what the desktop mounted and mounts nothing itself */
	const manage = state.can_mount && may("system");
	main.append(
		h("div", { class: "toolbar" }, h("h1", {}, tr("nav.mounts")),
			h("button", { class: "btn icon", title: tr("reload"), html: icon("refresh"), onclick: () => show().catch(fail) })),
		h("h2", {}, tr("mounts.drives")), drives,
		h("div", { class: "toolbar", style: "margin-top:24px" }, h("h2", { style: "margin:0;flex:1" }, tr("mounts.network_shares")),
			manage ? h("button", { class: "btn", onclick: () => addShare({}) }, tr("add")) : null,
			manage ? h("button", { class: "btn primary", onclick: (ev) => search(ev.currentTarget) }, tr("mounts.find")) : null),
		state.can_mount ? null : h("p", { class: "dim small" },
			tr("mounts.desktop_hint")),
		shares, found);

	/* a mount changes behind the page (boot, menu, another browser) */
	const refresh = () => { if (!document.hidden) show().catch(() => {}); };
	const timer = setInterval(refresh, 10000);
	document.addEventListener("visibilitychange", refresh);
	cleanup = () => {
		clearInterval(timer);
		document.removeEventListener("visibilitychange", refresh);
	};
}

/* ------------------------------------------------------------------ */
/* frame and routing                                                   */

const VIEWS = {
	remote: ["nav.remote", viewRemote, "read"],
	channels: ["nav.channels", viewChannels, "read"],
	epg: ["nav.guide", viewEpg, "read"],
	timers: ["nav.timers", viewTimers, "read"],
	recordings: ["nav.recordings", viewRecordings, "read"],
	mounts: ["nav.mounts", viewMounts, "read"],
	box: ["nav.box", viewBox, "read"],
	signin: ["signin.button", viewSignIn, "public"],
};

/* the picture on the TV with the OSD over it, saved as a PNG file */
async function screenshot(btn) {
	btn.disabled = true;
	try {
		const r = await fetch("/api/v1/osd/screenshot?osd=true&video=true&format=png", { credentials: "same-origin" });
		if (!r.ok) {
			let problem = {};
			try { problem = await r.json(); } catch { /* not JSON */ }
			throw new ApiError(r.status, problem);
		}
		const url = URL.createObjectURL(await r.blob());
		const now = new Date();
		const p = (n) => String(n).padStart(2, "0");
		const a = h("a", { href: url, download: `neutrino-${now.getFullYear()}${p(now.getMonth() + 1)}${p(now.getDate())}-${p(now.getHours())}${p(now.getMinutes())}${p(now.getSeconds())}.png` });
		document.body.append(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 10000);
		toast(tr("screenshot.saved"));
	} catch (e) {
		fail(e);
	} finally {
		btn.disabled = false;
	}
}

function frame() {
	const nav = h("nav", {}, Object.entries(VIEWS).filter(([k]) => k !== "signin").map(([k, [title]]) => {
		const a = h("a", { href: `#/${k}`, "data-view": k, html: `${icon(k)}<span></span>` });
		a.querySelector("span").textContent = tr(title);
		return a;
	}));
	const app = h("div", { class: "app" },
		h("header", { class: "top" },
			h("div", { class: "brand" }, h("img", { src: "/icons/icon.svg", alt: "" }), "Neutrino"),
			h("span", { class: "box", id: "boxname" }),
			h("span", { class: "spacer" }),
			h("button", { class: "btn icon", title: tr("screenshot.title"), html: icon("camera"), onclick: (ev) => screenshot(ev.currentTarget) }),
			h("a", { class: "btn icon", href: "/remote.html", title: tr("remote.only_phone"), html: icon("phone") })),
		nav,
		h("main", { id: "main" }));
	document.body.replaceChildren(app);
}

/* A page a plugin adds (ext/<id>/page.js): its default export gets the
 * main element and what it needs to run the command of the extension. */
const extensions = [];

async function viewExtension(main, id) {
	const ext = extensions.find((e) => e.id === id);
	if (!ext) {
		main.append(h("div", { class: "empty" }, tr("ext.missing")));
		return;
	}
	const mod = await import(`/ext/${id}/page.js`);
	await mod.default(main, {
		title: ext.title,
		run: async (args) => api(`ext/${id}/run`, { method: "POST", body: { args } }),
		h, icon, toast, fail, confirmDialog, tr, trn, language,
	});
}

async function loadExtensions() {
	if (!may("read"))
		return;
	try {
		extensions.push(...((await api("ext")).items || []).filter((e) => may(e.level)));
	} catch { /* an older box without extensions */ }
	const box = document.querySelector('nav a[data-view="box"]');
	for (const e of extensions)
		box.before(h("a", { href: `#/ext/${e.id}`, "data-view": `ext/${e.id}`, html: `${icon(e.icon)}<span></span>` }));
	for (const e of extensions)
		document.querySelector(`nav a[data-view="ext/${e.id}"] span`).textContent = e.title;
}

async function route() {
	cleanup?.();
	cleanup = null;
	const ext = (location.hash.match(/^#\/ext\/([\w-]+)/) || [])[1];
	if (ext) {
		for (const a of document.querySelectorAll("nav a"))
			a.classList.toggle("active", a.dataset.view === `ext/${ext}`);
		const main = $("#main");
		main.replaceChildren();
		main.classList.remove("fill");
		try {
			await viewExtension(main, ext);
		} catch (e) {
			fail(e);
		}
		return;
	}
	const name = (location.hash.match(/^#\/(\w+)/) || [])[1] || "remote";
	const [, view, needed] = VIEWS[name] || VIEWS.remote;
	for (const a of document.querySelectorAll("nav a"))
		a.classList.toggle("active", a.dataset.view === name);
	const main = $("#main");
	main.replaceChildren();
	main.classList.toggle("fill", name === "remote");
	if (!may(needed) && name !== "signin") {
		location.hash = "#/signin";
		return;
	}
	try {
		await view(main);
	} catch (e) {
		fail(e);
	}
}

async function start() {
	await loadTexts();
	frame();
	applyTheme();
	try {
		await session();
	} catch (e) {
		toast(e.message, true);
	}
	api("system/info").then((i) => { $("#boxname").textContent = i.hostname || ""; document.title = `${i.hostname || "Neutrino"} - Neutrino`; }, () => {});
	await loadExtensions();
	window.addEventListener("hashchange", route);
	route();
}

start();
