/* The ProSiebenSat.1 media library sends only the television sets it knows
 * to the Joyn app and shows the others a farewell picture. */
(function () {
	if (location.host != 'hbbtv.prosiebensat1puls4.com' || location.pathname.indexOf('/apps/mediathek/') < 0)
		return;
	var at = /atvat|[?&]standalone=at/.test(location.pathname + location.search);
	location.replace('https://live.prd.ott.s.joyn.de/?platform=hbbtv' + (at ? '&country=at' : ''));
})();
