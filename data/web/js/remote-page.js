// The remote control alone, filling the screen of a phone. Opened from the
// code the TV shows, the address carries a one-time code that pairs the
// phone first.

import { session, may, key, ApiError, pairWithCode, pairOnTv, httpsUrl, applyTheme, loadTexts, tr } from "./api.js";
import { loadLayout, drawRemote } from "./remote.js";

const msg = document.getElementById("msg");
const status = document.getElementById("status");

function show(text, ...actions) {
	msg.replaceChildren(document.createTextNode(text));
	for (const a of actions) {
		const b = document.createElement(a.href ? "a" : "button");
		b.className = "btn primary";
		b.textContent = a.text;
		if (a.href)
			b.href = a.href;
		else
			b.addEventListener("click", a.run);
		msg.append(document.createElement("br"), b);
	}
	msg.hidden = false;
}

let statusTimer = 0;
function note(text) {
	status.textContent = text;
	clearTimeout(statusTimer);
	statusTimer = setTimeout(() => { status.textContent = ""; }, 2500);
}

let wakeLock = null;
async function stayAwake() {
	try {
		if ("wakeLock" in navigator && !wakeLock) {
			wakeLock = await navigator.wakeLock.request("screen");
			wakeLock.addEventListener("release", () => { wakeLock = null; });
		}
	} catch { /* not offered here */ }
}

function fullScreen() {
	const d = document.documentElement;
	if (!document.fullscreenElement && d.requestFullscreen)
		d.requestFullscreen({ navigationUI: "hide" }).catch(() => {});
	stayAwake();
}

/* one button: full screen, and once there (or where a browser has no full
 * screen), the full interface */
const full = document.getElementById("full");
const canFull = () => !!document.documentElement.requestFullscreen && !document.fullscreenElement;

function updateFull() {
	full.textContent = canFull() ? tr("remote.full_screen") : tr("remote.full_interface");
}

function toggleFull() {
	if (canFull())
		fullScreen();
	else
		location.href = "/";
}

/* the first touch of a key goes full screen as well */
function firstTouch(ev) {
	if (ev.target.closest("#full"))
		return;
	document.removeEventListener("pointerdown", firstTouch);
	fullScreen();
}

function noAccess() {
	const secure = httpsUrl();
	if (secure)
		show(tr("phone.no_access_https"), { text: tr("phone.go_https"), href: secure });
	else if (location.protocol === "https:")
		show(tr("phone.no_access_pair"), { text: tr("pair.ask"), run: askTv });
	else
		show(tr("phone.no_access"), { text: tr("signin.button"), href: "/#/signin" });
}

async function askTv() {
	try {
		await pairOnTv((digits) => show(tr("phone.allow", { code: digits })));
		ready();
	} catch (e) {
		show(e.message || tr("pair.refused"), { text: tr("phone.ask_again"), run: askTv });
	}
}

async function press(name) {
	try {
		await key(name);
	} catch (e) {
		if (e instanceof ApiError && (e.status === 401 || e.status === 403))
			noAccess();
		else
			note(e.message || tr("phone.no_answer"));
	}
}

async function ready() {
	msg.hidden = true;
	if (!may("write")) {
		noAccess();
		return;
	}
	drawRemote(document.getElementById("remote"), await loadLayout(), press);
}

async function start() {
	await loadTexts();
	document.title = tr("remote.page_title");
	full.addEventListener("click", toggleFull);
	document.addEventListener("fullscreenchange", updateFull);
	document.addEventListener("pointerdown", firstTouch);
	updateFull();
	applyTheme();
	document.addEventListener("visibilitychange", () => { if (document.visibilityState === "visible" && document.fullscreenElement) stayAwake(); });
	try {
		await session();
	} catch {
		show(tr("phone.box_silent"));
		return;
	}
	const code = new URLSearchParams(location.search).get("pair");
	if (code) {
		history.replaceState(null, "", location.pathname);
		try {
			await pairWithCode(code);
			note(tr("phone.paired"));
		} catch (e) {
			/* the home network may let it in anyway, but it is not paired */
			show(tr("phone.not_paired", { why: e.message || tr("phone.pairing_failed") }),
				{ text: tr("pair.ask"), run: askTv }, { text: tr("phone.without_pairing"), run: ready });
			return;
		}
	}
	ready();
}

start();
