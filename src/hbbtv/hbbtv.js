/*
 * OIPF/HbbTV objects for neutrino-hbbtv. Injected at document start into
 * every frame. The methods live on the element prototypes so they work no
 * matter when an application inserts its <object> or <video> elements.
 *
 * Video is played by the box: WebKit never gets the source, and its state
 * comes from Neutrino through the launcher ("position", "ended", "error").
 * Where a video object or element sits, WebKit's hole punch player clears
 * the graphics down to the picture, as an HbbTV browser does.
 *
 * License: GPL-2.0-or-later
 */
(function () {
	'use strict';
	if (window.__neutrinoHbbtv)
		return;
	window.__neutrinoHbbtv = true;

	/* shown only when the launcher runs with NEUTRINO_HBBTV_DEBUG */
	function trace(name, args) {
		console.log('oipf: ' + name + '(' + Array.prototype.map.call(args, function (a) {
			try { return JSON.stringify(a); } catch (e) { return String(a); }
		}).join(', ') + ')');
	}

	function traced(obj) {
		for (var k in obj) {
			if (typeof obj[k] === 'function')
				obj[k] = (function (name, fn) {
					return function () { trace(name, arguments); return fn.apply(this, arguments); };
				})(k, obj[k]);
		}
		return obj;
	}

	var bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.neutrino;

	function post(line) {
		if (bridge)
			bridge.postMessage({ cmd: 'media', line: line });
	}

	/* a page's own script may take over what would go to the player */
	function send(line) {
		if (!window.__neutrinoPage || !window.__neutrinoPage.command(line, post))
			post(line);
	}
	window.__neutrinoCurrent = function () { return current; };

	/* dvb://current.ait/<organisation>.<application>, both in hex, is an
	 * application the channel signals; what follows goes to its address */
	function applicationUrl(uri) {
		var m = /^dvb:\/\/current\.ait\/([0-9a-f]+)\.([0-9a-f]+)([?#].*)?$/i.exec(uri);
		if (!m)
			return new URL(uri, document.baseURI).href;
		var url = (window.__neutrinoApps || {})[parseInt(m[1], 16) + '.' + parseInt(m[2], 16)];
		var rest = m[3] || '';
		if (!url)
			return null;
		if (rest.charAt(0) == '?' && url.indexOf('?') >= 0)
			rest = '&' + rest.substr(1);
		return url + rest;
	}

	var channel = window.__neutrinoChannel || {
		ccid: 'ccid:0', name: '', onid: 0, tsid: 0, sid: 0,
		channelType: 0, idType: 15, dsd: '', majorChannel: 0
	};

	var keys = {
		VK_LEFT: 37, VK_UP: 38, VK_RIGHT: 39, VK_DOWN: 40, VK_ENTER: 13,
		VK_BACK: 461, VK_RED: 403, VK_GREEN: 404, VK_YELLOW: 405, VK_BLUE: 406,
		VK_PLAY: 415, VK_PAUSE: 19, VK_PLAY_PAUSE: 402, VK_STOP: 413,
		VK_FAST_FWD: 417, VK_REWIND: 412, VK_TRACK_NEXT: 425, VK_TRACK_PREV: 424,
		VK_INFO: 457, VK_TELETEXT: 459,
		VK_0: 48, VK_1: 49, VK_2: 50, VK_3: 51, VK_4: 52,
		VK_5: 53, VK_6: 54, VK_7: 55, VK_8: 56, VK_9: 57
	};
	window.KeyEvent = window.KeyEvent || {};
	for (var k in keys) {
		window[k] = keys[k];
		window.KeyEvent[k] = keys[k];
	}

	var keyset = traced({
		RED: 0x1, GREEN: 0x2, YELLOW: 0x4, BLUE: 0x8, NAVIGATION: 0x10,
		VCR: 0x20, SCROLL: 0x40, INFO: 0x80, NUMERIC: 0x100, ALPHA: 0x200, OTHER: 0x400,
		value: 0x1f,
		setValue: function (v) {
			this.value = v;
			if (bridge)
				bridge.postMessage({ cmd: 'keyset', value: v });
			return v;
		},
		getMaximumValue: function () { return 0x1ff; }
	});

	var successor = false;
	var application = traced({
		privateData: { keyset: keyset, currentChannel: channel },
		visible: true,
		show: function () { this.visible = true; document.documentElement.style.visibility = ''; return true; },
		hide: function () { this.visible = false; document.documentElement.style.visibility = 'hidden'; return true; },
		activate: function () {},
		deactivate: function () {},
		/* an application hands over to another with createApplication() and then
		 * calls destroyApplication(); only one without a successor ends HbbTV */
		createApplication: function (uri) {
			var url = applicationUrl(uri);
			if (!url)
				return null;
			successor = true;
			window.location.href = url;
			return application;
		},
		destroyApplication: function () {
			if (!successor && bridge)
				bridge.postMessage({ cmd: 'exit' });
		},
		getOwnerApplication: function () { return application; }
	});

	var configuration = {
		preferredAudioLanguage: 'deu,eng',
		preferredSubtitleLanguage: 'deu,eng',
		preferredUILanguage: 'deu,eng',
		countryId: 'DEU',
		regionId: 0
	};

	var localSystem = { deviceID: 'neutrino', modelName: 'Neutrino', vendorName: 'Neutrino' };

	/* the live picture; Neutrino stops and resumes it around the videos itself */
	var broadcast = traced({
		playState: 2,
		fullScreen: true,
		bindToCurrentChannel: function () { this.playState = 2; return channel; },
		setChannel: function () {},
		prevChannel: function () {},
		nextChannel: function () {},
		release: function () {},
		stop: function () {},
		setFullScreen: function (f) { this.fullScreen = !!f; },
		getChannelConfig: function () {
			return { channelList: { length: 1, item: function () { return channel; },
				getChannelByTriplet: function () { return channel; } } };
		}
	});

	/* --- holes onto the picture --- */

	var nativeLoad = HTMLMediaElement.prototype.load;
	var nativeAppend = Node.prototype.appendChild;
	function nativeGetter(name) {
		return Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, name).get;
	}
	var nativeReadyState = nativeGetter('readyState');
	var nativeNetworkState = nativeGetter('networkState');
	var nativeError = nativeGetter('error');

	function holeSource() {
		var source = document.createElement('source');
		source.setAttribute('type', 'video/holepunch');
		source.setAttribute('src', 'data:,');
		source.setAttribute('data-neutrino-hole', '');
		return source;
	}

	/* a video object cannot load in WebKit, so its children are shown in its place */
	function punchObject(el) {
		if (el.__neutrinoHole)
			return;
		var video = document.createElement('video');
		video.setAttribute('style', 'position:absolute;left:0;top:0;width:100%;height:100%;');
		nativeAppend.call(video, holeSource());
		nativeAppend.call(el, video);
		el.__neutrinoHole = video;
		nativeLoad.call(video);
	}

	function punchMedia(el) {
		if (el.__neutrinoHole)
			return;
		el.removeAttribute('src');
		el.__neutrinoHole = holeSource();
		nativeAppend.call(el, el.__neutrinoHole);
		nativeLoad.call(el);
	}

	/* --- playback through Neutrino --- */

	var states = new WeakMap();
	var current = null;
	/* the video/broadcast object whose size the live picture follows */
	var broadcastElement = null;
	var lastWindow = '';

	function state(el) {
		var s = states.get(el);
		if (!s) {
			s = { src: '', started: false, paused: true, ended: false, time: 0, duration: NaN,
			      readyState: 0, playState: 0, speed: 0, fullScreen: false };
			states.set(el, s);
		}
		return s;
	}

	function windowOf(el) {
		/* an object shown through its fallback content has no box of its own */
		var hole = el instanceof HTMLObjectElement ? el.__neutrinoHole : null;
		var r = (hole || el).getBoundingClientRect();
		var w = window.innerWidth, h = window.innerHeight;
		var full = isBroadcast(el) ? broadcast.fullScreen : state(el).fullScreen;
		if (r.width <= 2 || r.height <= 2 || full ||
		    (r.left <= 0 && r.top <= 0 && r.right >= w && r.bottom >= h))
			return 'full';
		return Math.round(r.left) + ' ' + Math.round(r.top) + ' ' + Math.round(r.width) + ' ' + Math.round(r.height);
	}

	function followWindow() {
		var el = current || (broadcastElement && broadcastElement.isConnected ? broadcastElement : null);
		if (!el)
			return;
		var win = windowOf(el);
		if (win !== lastWindow) {
			lastWindow = win;
			send('window ' + win);
		}
	}
	setInterval(followWindow, 500);

	function start(el, url) {
		if (current && current !== el)
			stopCurrent();
		var s = state(el);
		current = el;
		if (el instanceof HTMLMediaElement)
			punchMedia(el);
		else
			punchObject(el);
		s.started = true;
		s.paused = false;
		s.ended = false;
		s.readyState = 0;
		lastWindow = windowOf(el);
		send('window ' + lastWindow);
		send('play ' + url);
		if (s.time > 0)
			send('seek ' + Math.round(s.time * 1000));
	}

	function stopCurrent() {
		var el = current;
		if (!el)
			return;
		var s = state(el);
		s.started = false;
		s.paused = true;
		current = null;
		lastWindow = '';
		send('stop');
		if (el instanceof HTMLObjectElement)
			avState(el, 0);
	}

	/* OIPF A/V control object: <object type="video/mp4" data="..."> */

	function avState(el, st) {
		var s = state(el);
		if (s.playState === st)
			return;
		s.playState = st;
		trace('PlayStateChange', [st]);
		if (typeof el.onPlayStateChange === 'function') {
			try { el.onPlayStateChange(st); } catch (e) { console.log(e); }
		}
		var ev = new Event('PlayStateChange');
		ev.state = st;
		el.dispatchEvent(ev);
	}

	function isBroadcast(el) {
		return /^video\/broadcast/i.test(el.getAttribute('type') || '');
	}

	var av = traced({
		play: function (speed) {
			var s = state(this);
			speed = speed === undefined ? 1 : Number(speed);
			s.speed = speed;
			if (this === current && s.started) {
				if (speed === 0 && !s.paused) {
					send('pause');
					s.paused = true;
					avState(this, 2);
				} else if (speed !== 0 && s.paused) {
					send('resume');
					s.paused = false;
					avState(this, 1);
				}
				return true;
			}
			if (speed === 0)
				return true;
			var url = this.getAttribute('data');
			if (!url)
				return false;
			start(this, new URL(url, document.baseURI).href);
			avState(this, 3);
			return true;
		},
		stop: function () {
			if (this === current)
				stopCurrent();
			else
				avState(this, 0);
			return true;
		},
		seek: function (ms) {
			var s = state(this);
			s.time = Number(ms) / 1000;
			if (this === current && s.started)
				send('seek ' + Math.round(Number(ms)));
			return true;
		}
	});

	var objectMethods = {
		getOwnerApplication: function () { return application; },
		play: function (speed) { return isBroadcast(this) ? false : av.play.call(this, speed); },
		stop: function () { return isBroadcast(this) ? broadcast.stop() : av.stop.call(this); },
		seek: function (ms) { return isBroadcast(this) ? false : av.seek.call(this, ms); },
		setFullScreen: function (f) {
			if (isBroadcast(this)) {
				broadcast.setFullScreen(f);
				broadcastElement = this;
				punchObject(this);
			} else
				state(this).fullScreen = !!f;
			followWindow();
		},
		bindToCurrentChannel: function () {
			broadcastElement = this;
			punchObject(this);
			followWindow();
			return broadcast.bindToCurrentChannel();
		},
		setChannel: function (c) { return broadcast.setChannel(c); },
		prevChannel: function () { return broadcast.prevChannel(); },
		nextChannel: function () { return broadcast.nextChannel(); },
		release: function () { return isBroadcast(this) ? broadcast.release() : av.stop.call(this); },
		getChannelConfig: function () { return broadcast.getChannelConfig(); },
		selectComponent: function () {},
		unselectComponent: function () {},
		getComponents: function () { return []; }
	};
	var objectProperties = {
		configuration: function () { return configuration; },
		localSystem: function () { return localSystem; },
		currentChannel: function () { return channel; },
		playState: function () { return isBroadcast(this) ? broadcast.playState : state(this).playState; },
		fullScreen: function () { return isBroadcast(this) ? broadcast.fullScreen : state(this).fullScreen; },
		playPosition: function () { return Math.round(state(this).time * 1000); },
		playTime: function () { var d = state(this).duration; return isNaN(d) ? 0 : Math.round(d * 1000); },
		speed: function () { return state(this).speed; },
		error: function () { return state(this).playState === 6 ? 0 : undefined; }
	};
	var op = HTMLObjectElement.prototype;
	for (var m in objectMethods)
		Object.defineProperty(op, m, { value: objectMethods[m], configurable: true, writable: true });
	for (var p in objectProperties)
		Object.defineProperty(op, p, { get: objectProperties[p], configurable: true });

	window.oipfObjectFactory = {
		isObjectSupported: function (t) {
			return /^(application\/oipf|video\/broadcast|video\/mp4|video\/mpeg|application\/dash\+xml)/i.test(t);
		},
		createApplicationManagerObject: function () { return { getOwnerApplication: function () { return application; } }; },
		createConfigurationObject: function () { return { configuration: configuration, localSystem: localSystem }; },
		createVideoBroadcastObject: function () {
			var o = document.createElement('object');
			o.setAttribute('type', 'video/broadcast');
			return o;
		},
		createVideoMpegObject: function () {
			var o = document.createElement('object');
			o.setAttribute('type', 'video/mp4');
			return o;
		}
	};

	/* HTML5 <video> and <audio> */

	function fire(el, type) {
		el.dispatchEvent(new Event(type));
	}

	function sourceOf(el) {
		var s = state(el);
		if (s.src)
			return s.src;
		var source = el.querySelector('source[src]:not([data-neutrino-hole])');
		return source ? source.src : '';
	}

	/* WebKit's own media events would tell of a load that never happens */
	['loadstart', 'progress', 'suspend', 'abort', 'error', 'emptied', 'stalled', 'loadedmetadata',
	 'loadeddata', 'canplay', 'canplaythrough', 'playing', 'waiting', 'seeking', 'seeked', 'ended',
	 'durationchange', 'timeupdate', 'play', 'pause', 'ratechange'].forEach(function (type) {
		window.addEventListener(type, function (e) {
			if (e.isTrusted && e.target instanceof HTMLMediaElement)
				e.stopImmediatePropagation();
		}, true);
	});

	var mp = HTMLMediaElement.prototype;
	var mediaMethods = {
		play: function () {
			var s = state(this), src = sourceOf(this);
			if (!src)
				return Promise.reject(new DOMException('no source', 'NotSupportedError'));
			if (this === current && s.started) {
				if (s.paused)
					send('resume');
			} else
				start(this, src);
			s.paused = false;
			fire(this, 'play');
			return Promise.resolve();
		},
		pause: function () {
			var s = state(this);
			if (this === current && !s.paused)
				send('pause');
			s.paused = true;
			fire(this, 'pause');
		},
		load: function () {
			if (this === current)
				stopCurrent();
			var s = state(this);
			s.readyState = 0;
			s.time = 0;
			s.duration = NaN;
			s.ended = false;
			if (this.autoplay && sourceOf(this))
				this.play();
		},
		canPlayType: function (t) {
			return /mp4|mpegurl|dash|mp2t|mpeg/i.test(t) ? 'probably' : '';
		}
	};
	for (var mm in mediaMethods)
		Object.defineProperty(mp, mm, { value: mediaMethods[mm], configurable: true, writable: true });

	Object.defineProperty(mp, 'src', {
		get: function () { return state(this).src; },
		set: function (v) {
			var s = state(this);
			if (this === current)
				stopCurrent();
			s.src = v ? new URL(v, document.baseURI).href : '';
			s.time = 0;
			s.duration = NaN;
			s.readyState = 0;
			if (s.src && this.autoplay)
				this.play();
		},
		configurable: true
	});
	Object.defineProperty(mp, 'currentTime', {
		get: function () { return state(this).time; },
		set: function (v) {
			var s = state(this);
			s.time = Number(v) || 0;
			if (this === current && s.started) {
				fire(this, 'seeking');
				send('seek ' + Math.round(s.time * 1000));
				fire(this, 'seeked');
			}
		},
		configurable: true
	});
	var mediaProperties = {
		currentSrc: function () { return sourceOf(this); },
		duration: function () { return state(this).duration; },
		paused: function () { return state(this).paused; },
		ended: function () { return state(this).ended; },
		readyState: function () { return state(this).readyState; },
		networkState: function () { return state(this).started ? 2 : 1; },
		error: function () { return null; }
	};
	for (var mq in mediaProperties)
		Object.defineProperty(mp, mq, { get: mediaProperties[mq], configurable: true });

	var setAttribute = Element.prototype.setAttribute;
	Element.prototype.setAttribute = function (name, value) {
		if (this instanceof HTMLMediaElement && String(name).toLowerCase() === 'src') {
			this.src = value;
			return;
		}
		return setAttribute.call(this, name, value);
	};

	/* --- dash.js --- */

	/* dash.js needs media source extensions, which this WebKit is built without: pages get this
	 * stand-in, which hands the manifest to Neutrino through their video element */
	var dashEvents = {
		CAN_PLAY: 'canPlay', ERROR: 'error', PLAYBACK_ENDED: 'playbackEnded', PLAYBACK_ERROR: 'playbackError',
		PLAYBACK_NOT_ALLOWED: 'playbackNotAllowed', PLAYBACK_PAUSED: 'playbackPaused',
		PLAYBACK_PLAYING: 'playbackPlaying', PLAYBACK_SEEKED: 'playbackSeeked',
		PLAYBACK_SEEKING: 'playbackSeeking', PLAYBACK_TIME_UPDATED: 'playbackTimeUpdated',
		PLAYBACK_WAITING: 'playbackWaiting', TEXT_TRACKS_ADDED: 'allTextTracksAdded'
	};
	var dashRelay = {
		canplay: dashEvents.CAN_PLAY, ended: dashEvents.PLAYBACK_ENDED, error: dashEvents.ERROR,
		pause: dashEvents.PLAYBACK_PAUSED, playing: dashEvents.PLAYBACK_PLAYING,
		seeked: dashEvents.PLAYBACK_SEEKED, seeking: dashEvents.PLAYBACK_SEEKING,
		timeupdate: dashEvents.PLAYBACK_TIME_UPDATED, waiting: dashEvents.PLAYBACK_WAITING
	};

	function dashPlayer() {
		var video = null, handlers = {}, autoplay = true;

		function emit(type, e) {
			e.type = type;
			(handlers[type] || []).slice().forEach(function (h) { h(e); });
		}
		function relay(e) {
			emit(dashRelay[e.type], e.type === 'error' ? { error: { code: 0, message: 'playback failed' } } : {});
		}
		function nothing() {}

		var player = {
			initialize: function (view, url, auto, start) {
				autoplay = auto !== false;
				if (view)
					player.attachView(view);
				if (url)
					player.attachSource(url, start);
			},
			attachView: function (view) {
				video = view;
				video.autoplay = false;
				for (var t in dashRelay)
					video.addEventListener(t, relay);
			},
			attachSource: function (url, start) {
				video.src = url || '';
				if (start)
					video.currentTime = start;
				if (url && autoplay)
					video.play();
			},
			destroy: function () {
				if (!video)
					return;
				video.src = '';
				for (var t in dashRelay)
					video.removeEventListener(t, relay);
				video = null;
			},
			on: function (type, h) { (handlers[type] = handlers[type] || []).push(h); },
			off: function (type, h) {
				var list = handlers[type] || [], i = list.indexOf(h);
				if (i >= 0)
					list.splice(i, 1);
			},
			play: function () { video.play(); },
			pause: function () { video.pause(); },
			seek: function (t) { video.currentTime = t; },
			time: function () { return video ? video.currentTime : 0; },
			duration: function () { return video ? video.duration : NaN; },
			isPaused: function () { return !video || video.paused; },
			getDVRWindowSize: function () { return Infinity; },
			getTracksFor: function () { return []; },
			getCurrentTrackFor: function () { return null; },
			updateSettings: nothing,
			attachVttRenderingDiv: nothing,
			setCurrentTrack: nothing,
			setTextTrack: nothing
		};
		return player;
	}

	var dashjs = { MediaPlayer: function () { return { create: dashPlayer }; } };
	dashjs.MediaPlayer.events = dashEvents;
	dashjs.MediaPlayer.errors = {};
	/* a page that loads the real library anyway keeps the stand-in */
	Object.defineProperty(window, 'dashjs', { get: function () { return dashjs; }, set: function () {}, configurable: true });

	/* --- what Neutrino reports back --- */

	window.__neutrinoMedia = function (line) {
		var el = current;
		if (!el)
			return;
		var s = state(el);
		var p = line.split(' ');
		var object = el instanceof HTMLObjectElement;
		if (p[0] === 'position') {
			var duration = Number(p[2]) / 1000;
			var first = s.readyState < 4;
			s.time = Number(p[1]) / 1000;
			if (first || (duration && duration !== s.duration))
				s.duration = duration || NaN;
			s.readyState = 4;
			if (object) {
				/* a report sent before a pause arrived must not undo it */
				if (p[3] === 'playing' && !s.paused && s.playState !== 1)
					avState(el, 1);
				return;
			}
			if (first) {
				['durationchange', 'loadedmetadata', 'loadeddata', 'canplay', 'canplaythrough', 'playing']
					.forEach(function (t) { fire(el, t); });
			}
			fire(el, 'timeupdate');
		} else if (p[0] === 'ended' || p[0] === 'error') {
			s.started = false;
			s.paused = true;
			current = null;
			lastWindow = '';
			if (object)
				avState(el, p[0] === 'ended' ? 5 : 6);
			else if (p[0] === 'ended') {
				s.ended = true;
				fire(el, 'pause');
				fire(el, 'ended');
			} else
				fire(el, 'error');
		}
	};

	/* what the video objects look like, for NEUTRINO_HBBTV_DEBUG */
	window.__neutrinoDump = function () {
		var list = document.querySelectorAll('object, video');
		for (var i = 0; i < list.length; i++) {
			var el = list[i], r = el.getBoundingClientRect(), hole = el.__neutrinoHole;
			var parent = el.parentElement;
			console.log('dump: ' + el.tagName + ' type=' + el.getAttribute('type') +
				' rect=' + Math.round(r.left) + ',' + Math.round(r.top) + ',' + Math.round(r.width) + 'x' + Math.round(r.height) +
				' display=' + getComputedStyle(el).display + ' parent=' + (parent ? parent.tagName + '.' + parent.className : '-') +
				' hole=' + (hole ? hole.tagName : 'none') +
				(hole && hole.tagName === 'VIDEO' ? ' hole-rect=' + JSON.stringify(hole.getBoundingClientRect()) +
					' ready=' + nativeReadyState.call(hole) + ' network=' + nativeNetworkState.call(hole) +
					' error=' + (nativeError.call(hole) ? nativeError.call(hole).code : 'none') : '') +
				(el === broadcastElement ? ' [broadcast]' : '') + (el === current ? ' [current]' : ''));
		}
	};

	/* what a remote client reads with tv_screen: the page, what the
	 * application highlights and the text on screen */
	window.__neutrinoScreen = function () {
		function text(el) {
			return (el.innerText || '').replace(/\s+/g, ' ').trim();
		}
		var out = [];
		if (document.title)
			out.push('Page: ' + document.title);
		out.push('Address: ' + location.href);
		var marked = [];
		var a = document.activeElement;
		if (a && a !== document.body && text(a))
			marked.push(text(a));
		var mark = /(^|[\s_-])(focus|focused|selected|active|current|highlight)($|[\s_-])/i;
		var list = document.querySelectorAll('[class]');
		for (var i = 0; i < list.length && marked.length < 5; i++) {
			var el = list[i];
			if (typeof el.className !== 'string' || !mark.test(el.className) || !el.getClientRects().length)
				continue;
			var t = text(el);
			if (t && t.length < 200 && marked.indexOf(t) < 0)
				marked.push(t);
		}
		if (marked.length)
			out.push('Highlighted: ' + marked.join(' | '));
		var body = document.body ? document.body.innerText.replace(/[ \t]+/g, ' ').replace(/\s*\n\s*/g, '\n').trim() : '';
		if (body.length > 2000)
			body = body.substr(0, 2000) + ' ...';
		if (body)
			out.push('Text:\n' + body);
		return out.join('\n');
	};

	/* an application that ends or removes its player stops the video */
	window.addEventListener('pagehide', stopCurrent);
	new MutationObserver(function () {
		if (current && !current.isConnected)
			stopCurrent();
	}).observe(document, { childList: true, subtree: true });
})();
