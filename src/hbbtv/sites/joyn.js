/* Joyn: its player needs Media Source and EME, which the browser lacks. The
 * playlist the app fetches names the DASH manifest and the license address;
 * Neutrino plays that with Widevine, and the app is hidden meanwhile. Back
 * or exit ends it, and the app gets a back of its own to leave its player,
 * which has given up by then. */
(function () {
	if (location.host != 'live.prd.ott.s.joyn.de')
		return;

	var bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.neutrino;
	var playing = false, paused = false, position = 0, duration = 0;
	var BACK = [8, 27, 413, 461];
	var BACKWARD = [37, 412], FORWARD = [39, 417], PAUSE = 19, PLAY = 415, TOGGLE = [13, 179, 402];

	function post(line) {
		if (bridge)
			bridge.postMessage({ cmd: 'media', line: line });
	}

	function show(on) {
		document.documentElement.style.visibility = on ? '' : 'hidden';
	}

	/* the HbbTV back key, which the app takes as leaving the player */
	function backToApp() {
		['keydown', 'keyup'].forEach(function (type) {
			var e = new KeyboardEvent(type, { bubbles: true, cancelable: true });
			Object.defineProperty(e, 'keyCode', { get: function () { return 461; } });
			Object.defineProperty(e, 'which', { get: function () { return 461; } });
			(document.activeElement || document.body || document).dispatchEvent(e);
		});
	}

	function start(p) {
		playing = true;
		paused = false;
		position = duration = 0;
		show(false);
		post('drm ' + p.licenseUrl + (p.certificateUrl ? ' ' + p.certificateUrl : ''));
		post('window full');
		post('play ' + p.manifestUrl);
	}

	var fetch0 = window.fetch;
	window.fetch = function (input) {
		var url = String(input && input.url ? input.url : input);
		var answer = fetch0.apply(this, arguments);
		if (!/\/playlist(\?|$)/.test(url))
			return answer;
		return answer.then(function (r) {
			r.clone().json().then(function (p) {
				if (p && p.manifestUrl && p.licenseUrl && p.streamingFormat == 'dash')
					start(p);
			}, function () {});
			return r;
		});
	};

	var media = window.__neutrinoMedia;
	window.__neutrinoMedia = function (line) {
		if (playing) {
			var p = line.split(' ');
			if (p[0] == 'position') {
				position = Number(p[1]);
				duration = Number(p[2]);
			} else if (p[0] == 'ended' || p[0] == 'error') {
				playing = false;
				show(true);
			}
		}
		if (media)
			media(line);
	};

	/* what was taken while playing stays away from the app when released */
	var held = {};
	window.addEventListener('keyup', function (e) {
		if (!held[e.keyCode])
			return;
		delete held[e.keyCode];
		e.stopImmediatePropagation();
		e.preventDefault();
	}, true);

	window.addEventListener('keydown', function (e) {
		if (!playing)
			return;
		var k = e.keyCode;
		held[k] = true;
		e.stopImmediatePropagation();
		e.preventDefault();
		if (BACK.indexOf(k) >= 0) {
			playing = false;
			post('stop');
			/* hidden until the app has left its player and the error it
			 * shows there */
			setTimeout(backToApp, 0);
			setTimeout(function () { show(true); }, 750);
			return;
		}
		if ((BACKWARD.indexOf(k) >= 0 || FORWARD.indexOf(k) >= 0) && duration > 0) {
			position = Math.max(0, Math.min(duration - 1000, position + (FORWARD.indexOf(k) >= 0 ? 30000 : -10000)));
			post('seek ' + position);
		} else if (k == PAUSE || k == PLAY || TOGGLE.indexOf(k) >= 0) {
			paused = k == PAUSE ? true : k == PLAY ? false : !paused;
			post(paused ? 'pause' : 'resume');
		}
	}, true);
})();
