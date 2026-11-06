// ROTK launcher window (owner phase 2: "лаунчер: мова UA/EN, 120 fps на ProMotion, гамма, Play,
// арт гри"). JXA + AppKit, run by Contents/MacOS/ROTK before the game:
//   osascript -l JavaScript launcher.js <settings.toml> [art.png]
// Shows the game art (Resources/launcher/art.png: the title screen the installer shoots from the
// player's own disc and upscales 4x, tools/launcher/make_art.sh), language (Українська / English),
// frame rate (60; 120 when a display can do it, ProMotion, even while it is set to 60 Hz: then one
// line under the list says how to switch ProMotion on), gamma (Оригінал / Сучасна / Тепла (2.5):
// original | modern | warm, src/polish/postfx/postfx.mm) and Play.
// Play writes [launcher] language / fps / gamma into settings.toml (other keys kept) and prints
// "play"; closing the window prints "quit". ROTK_LAUNCHER_AUTO=play|quit answers without the
// window (tests). ROTK_DISPLAY_HZ=<current>,<max>: the displays' refresh rate now and the most they
// can do (Contents/MacOS/ROTK reads all display modes; NSScreen only knows the current maximum).
// ROTK_LAUNCHER_SNAPSHOT=<png> draws the window off screen into a PNG and prints the offered rates,
// whether the hint shows and any cut-off label (tests/scripts/launcher_ui.sh).
// Gamepad (owner test dcb2fc2): D-pad up/down picks Language / Frame rate / Gamma / Play, left/right
// changes the picked list, A plays, B quits (GameController, read even while not frontmost).
// ROTK_LAUNCHER_PAD_SCRIPT="down down right a" feeds those presses to the same handler (tests).
// Language (owner 2026-11-06: «мову обирають в лончері»): English unless settings.toml says
// language = "uk"; the window's own text follows the language list, and Play saves the choice
// (the game, Contents/MacOS/ROTK, and the installer read the same setting).
ObjC.import('AppKit');
ObjC.import('Foundation');
ObjC.import('GameController');

function readText(path) {
  const s = $.NSString.stringWithContentsOfFileEncodingError(path, $.NSUTF8StringEncoding, null);
  return s.isNil() ? '' : s.js;
}
function writeText(path, text) {
  $(text).writeToFileAtomicallyEncodingError(path, true, $.NSUTF8StringEncoding, null);
}
function getSetting(text, key, def) {
  const m = text.match(new RegExp('^\\s*' + key + '\\s*=\\s*"?([^"\\n]*)"?', 'm'));
  return m ? m[1].trim() : def;
}
function putSettings(text, kv) {
  let lines = text.split('\n');
  if (!lines.some(l => l.trim() === '[launcher]')) lines = ['[launcher]'].concat(lines);
  for (const k of Object.keys(kv)) {
    const line = k + ' = "' + kv[k] + '"';
    const i = lines.findIndex(l => new RegExp('^\\s*' + k + '\\s*=').test(l));
    if (i >= 0) lines[i] = line;
    else lines.splice(lines.findIndex(l => l.trim() === '[launcher]') + 1, 0, line);
  }
  return lines.join('\n').replace(/\n*$/, '\n');
}
function displayHz() {  // {cur, max}
  const env = $.NSProcessInfo.processInfo.environment.objectForKey('ROTK_DISPLAY_HZ');
  if (!env.isNil()) {
    const [c, m] = env.js.split(',').map(v => parseInt(v, 10) || 60);
    return { cur: c, max: Math.max(c, m || c) };
  }
  let hz = 60;
  const screens = $.NSScreen.screens;
  for (let i = 0; i < screens.count; i++) {
    const s = screens.objectAtIndex(i);
    if (s.respondsToSelector('maximumFramesPerSecond')) hz = Math.max(hz, s.maximumFramesPerSecond);
  }
  return { cur: hz, max: hz };
}

// Numbers from AppKit come back through the JXA bridge as strings ("1" !== 1, PIT-D02): the
// popups' selected index always goes through Number().
const sel = (popup) => Number(popup.indexOfSelectedItem);
const GAMMAS = ['original', 'modern', 'warm'];  // owner test 049f010: a choice of three, no slider

function run(argv) {
  const settingsPath = argv[0], artPath = argv[1] || '';
  const text = readText(settingsPath);
  const dh = displayHz(), can120 = dh.max >= 120, hint = can120 && dh.cur < 120;
  const st = {
    language: getSetting(text, 'language', 'en') === 'uk' ? 'uk' : 'en',
    fps: getSetting(text, 'fps', dh.cur >= 120 ? '120' : '60') === '120' && can120 ? '120' : '60',
    gamma: GAMMAS.includes(getSetting(text, 'gamma', 'original')) ? getSetting(text, 'gamma', 'original') : 'original',
  };
  const save = () => writeText(settingsPath, putSettings(text, st));
  const auto = $.NSProcessInfo.processInfo.environment.objectForKey('ROTK_LAUNCHER_AUTO');
  if (!auto.isNil()) {
    if (auto.js === 'play') save();
    return auto.js === 'play' ? 'play' : 'quit';
  }

  const T = {
    uk: { lang: 'Мова', fps: 'Частота кадрів', gamma: 'Гамма', gammas: ['Оригінал', 'Сучасна', 'Тепла (2.5)'], play: 'Грати', quit: 'Вийти',
          hint: 'Для 120 fps увімкніть ProMotion: Системні параметри → Дисплеї → Частота оновлення → ProMotion.' },
    en: { lang: 'Language', fps: 'Frame rate', gamma: 'Gamma', gammas: ['Original', 'Modern', 'Warm (2.5)'], play: 'Play', quit: 'Quit',
          hint: 'For 120 fps turn ProMotion on: System Settings → Displays → Refresh Rate → ProMotion.' },
  };
  const app = $.NSApplication.sharedApplication;
  app.setActivationPolicy($.NSApplicationActivationPolicyRegular);
  const W = 768, P = 130, H = 576 + P;  // art 4:3 above (the 640x480 title, 4x), controls below
  const win = $.NSWindow.alloc.initWithContentRectStyleMaskBackingDefer(
    $.NSMakeRect(0, 0, W, H), $.NSWindowStyleMaskTitled | $.NSWindowStyleMaskClosable,
    $.NSBackingStoreBuffered, false);
  win.title = 'The Lord of the Rings: The Return of the King';
  win.center;
  const view = win.contentView;
  win.backgroundColor = $.NSColor.blackColor;  // (a CGColor through the JXA bridge kills osascript)

  let artInfo = 'art: none';
  if (artPath) {
    const img = $.NSImage.alloc.initWithContentsOfFile(artPath);
    if (!img.isNil()) {
      const iv = $.NSImageView.alloc.initWithFrame($.NSMakeRect(0, P, W, H - P));
      iv.image = img;
      iv.imageScaling = $.NSImageScaleProportionallyUpOrDown;  // never stretched
      view.addSubview(iv);
      artInfo = 'art: view ' + W + 'x' + (H - P) + ', image ' + img.size.width + 'x' + img.size.height;
    }
  }
  // bottom panel
  const panel = $.NSVisualEffectView.alloc.initWithFrame($.NSMakeRect(0, 0, W, P));
  panel.material = $.NSVisualEffectMaterialHUDWindow;
  panel.blendingMode = $.NSVisualEffectBlendingModeWithinWindow;
  panel.state = $.NSVisualEffectStateActive;
  view.addSubview(panel);

  const labels = [];
  const label = (x, y, w, h) => {
    const l = $.NSTextField.wrappingLabelWithString('');
    l.frame = $.NSMakeRect(x, y, w, h || 20);
    l.textColor = $.NSColor.whiteColor;
    panel.addSubview(l);
    labels.push(l);
    return l;
  };
  const lLang = label(24, 98, 160), lFps = label(204, 98, 150), lGamma = label(364, 98, 160);
  const lHint = label(24, 8, 500, 46);
  lHint.font = $.NSFont.systemFontOfSize(12);
  lHint.textColor = $.NSColor.colorWithWhiteAlpha(0.82, 1);
  lHint.hidden = !hint;

  const pLang = $.NSPopUpButton.alloc.initWithFramePullsDown($.NSMakeRect(20, 62, 166, 26), false);
  pLang.addItemsWithTitles($(['Українська', 'English']));
  pLang.selectItemAtIndex(st.language === 'en' ? 1 : 0);
  panel.addSubview(pLang);

  const pFps = $.NSPopUpButton.alloc.initWithFramePullsDown($.NSMakeRect(200, 62, 140, 26), false);
  pFps.addItemsWithTitles($(can120 ? ['60 fps', '120 fps'] : ['60 fps']));
  pFps.selectItemAtIndex(st.fps === '120' ? 1 : 0);
  panel.addSubview(pFps);

  const pGamma = $.NSPopUpButton.alloc.initWithFramePullsDown($.NSMakeRect(360, 62, 160, 26), false);
  pGamma.addItemsWithTitles($(T.uk.gammas));
  pGamma.selectItemAtIndex(GAMMAS.indexOf(st.gamma));
  panel.addSubview(pGamma);

  // Buttons take the first click even while the window is not active (an inactive launcher window
  // swallowed the click on «Грати»: macOS 14 does not always let osascript become frontmost).
  ObjC.registerSubclass({
    name: 'RotkButton', superclass: 'NSButton',
    methods: { 'acceptsFirstMouse:': { types: ['bool', ['id']], implementation: function () { return true; } } },
  });
  const bPlay = $.RotkButton.buttonWithTitleTargetAction('', null, null);
  bPlay.frame = $.NSMakeRect(W - 222, 58, 200, 50);
  bPlay.bezelStyle = $.NSBezelStyleRegularSquare;
  bPlay.keyEquivalent = '\r';
  bPlay.font = $.NSFont.boldSystemFontOfSize(22);
  panel.addSubview(bPlay);
  const bQuit = $.RotkButton.buttonWithTitleTargetAction('', null, null);
  bQuit.frame = $.NSMakeRect(W - 222, 18, 200, 28);
  bQuit.keyEquivalent = '\x1b';
  panel.addSubview(bQuit);

  const relabel = () => {
    const t = T[sel(pLang) === 1 ? 'en' : 'uk'];
    lLang.stringValue = t.lang;
    lFps.stringValue = t.fps;
    lHint.stringValue = t.hint;
    lGamma.stringValue = t.gamma;
    t.gammas.forEach((g, i) => { pGamma.itemAtIndex(i).title = g; });
    bPlay.title = t.play;
    bQuit.title = t.quit;
  };
  relabel();

  let choice = 'quit';
  // gamepad: rows of controls, the picked one is the first responder (focus ring)
  const rows = [pLang, pFps, pGamma, bPlay];
  let row = 3;
  const pick = (r) => { row = Math.max(0, Math.min(rows.length - 1, r)); win.makeFirstResponder(rows[row]); };
  const press = (k) => {
    if (k === 'up') pick(row - 1);
    else if (k === 'down') pick(row + 1);
    else if ((k === 'left' || k === 'right') && row < 3) {
      const pu = rows[row], n = Number(pu.numberOfItems);
      pu.selectItemAtIndex(Math.max(0, Math.min(n - 1, sel(pu) + (k === 'left' ? -1 : 1))));
      relabel();
    } else if (k === 'a') { choice = 'play'; app.stopModalWithCode(1); }
    else if (k === 'b') app.stopModalWithCode(0);
  };
  const env = $.NSProcessInfo.processInfo.environment;
  const scriptEnv = env.objectForKey('ROTK_LAUNCHER_PAD_SCRIPT');
  const script = scriptEnv.isNil() ? null : scriptEnv.js.split(/\s+/).filter(Boolean);
  if ($.GCController.respondsToSelector('setShouldMonitorBackgroundEvents:')) $.GCController.shouldMonitorBackgroundEvents = true;
  const held = {};
  const tick = () => {
    if (script) { if (script.length) press(script.shift()); return; }
    const cs = $.GCController.controllers;
    const now = {};
    for (let i = 0; i < cs.count; i++) {
      const g = cs.objectAtIndex(i).extendedGamepad;
      if (g.isNil()) continue;
      const on = (b) => !b.isNil() && b.isPressed;
      if (on(g.buttonA)) now.a = 1;
      if (on(g.buttonB)) now.b = 1;
      if (on(g.dpad.up) || g.leftThumbstick.yAxis.value > 0.6) now.up = 1;
      if (on(g.dpad.down) || g.leftThumbstick.yAxis.value < -0.6) now.down = 1;
      if (on(g.dpad.left) || g.leftThumbstick.xAxis.value < -0.6) now.left = 1;
      if (on(g.dpad.right) || g.leftThumbstick.xAxis.value > 0.6) now.right = 1;
    }
    for (const k of ['up', 'down', 'left', 'right', 'b', 'a']) {
      if (now[k] && !held[k]) press(k);
      held[k] = now[k];
    }
  };
  // Controls report through one target; the modal session ends with Play (1) or Quit/close (0).
  ObjC.registerSubclass({
    name: 'RotkLauncherTarget',
    methods: {
      'changed:': { types: ['void', ['id']], implementation: function () { relabel(); } },
      'tick:': { types: ['void', ['id']], implementation: function () { tick(); } },
      'play:': { types: ['void', ['id']], implementation: function () { choice = 'play'; app.stopModalWithCode(1); } },
      'quit:': { types: ['void', ['id']], implementation: function () { app.stopModalWithCode(0); } },
      'windowWillClose:': { types: ['void', ['id']], implementation: function () { app.stopModalWithCode(0); } },
    },
  });
  const tgt = $.RotkLauncherTarget.alloc.init;
  for (const c of [pLang, pFps, pGamma]) { c.target = tgt; c.action = 'changed:'; }
  bPlay.target = tgt; bPlay.action = 'play:';
  bQuit.target = tgt; bQuit.action = 'quit:';
  win.delegate = tgt;

  // ROTK_LAUNCHER_SNAPSHOT=<png>: draw the window off screen into a PNG and quit (tests)
  const snap = $.NSProcessInfo.processInfo.environment.objectForKey('ROTK_LAUNCHER_SNAPSHOT');
  if (!snap.isNil()) {
    const rep = view.bitmapImageRepForCachingDisplayInRect(view.bounds);
    view.cacheDisplayInRectToBitmapImageRep(view.bounds, rep);
    rep.representationUsingTypeProperties($.NSBitmapImageFileTypePNG, $({})).writeToFileAtomically(snap.js, true);
    // a label is cut off when its text needs more height than its frame at the frame's width
    const clipped = labels.filter(l => !l.hidden && l.stringValue.js &&
      l.cell.cellSizeForBounds($.NSMakeRect(0, 0, l.frame.size.width, 10000)).height > l.frame.size.height + 0.5)
      .map(l => l.stringValue.js.slice(0, 24));
    const t = T[sel(pLang) === 1 ? 'en' : 'uk'];
    return ['snapshot', artInfo, 'language: ' + (sel(pLang) === 1 ? 'en' : 'uk'),
            'labels: ' + [t.lang, t.fps, t.gamma, t.play, t.quit].join('|'), 'hint text: ' + t.hint, 'gamma: ' + T.uk.gammas.join('|'), 'fps: ' + (can120 ? '60,120' : '60'), 'hint: ' + (hint ? 'yes' : 'no'),
            'clipped: ' + (clipped.length ? clipped.join(' | ') : 'none')].join('\n');
  }
  app.activateIgnoringOtherApps(true);
  win.makeKeyAndOrderFront(null);
  pick(3);
  const timer = $.NSTimer.timerWithTimeIntervalTargetSelectorUserInfoRepeats(script ? 0.25 : 1 / 30, tgt, 'tick:', null, true);
  $.NSRunLoop.currentRunLoop.addTimerForMode(timer, $.NSRunLoopCommonModes);
  // The choice is kept in JS: runModalForWindow's NSModalResponse comes back through the JXA
  // bridge as a string ("1" !== 1 made every Play a quit, owner test 049f010).
  app.runModalForWindow(win);
  if (choice !== 'play') return 'quit';
  st.language = sel(pLang) === 1 ? 'en' : 'uk';
  st.fps = sel(pFps) === 1 ? '120' : '60';
  st.gamma = GAMMAS[sel(pGamma)] || 'original';
  save();
  win.orderOut(null);
  // macOS 14 cooperative activation: the game (Contents/MacOS/ROTK after execv) becomes the active
  // app only if this frontmost one yields to it (owner test dcb2fc2: pad dead in an unfocused game).
  const owner = $.NSProcessInfo.processInfo.environment.objectForKey('ROTK_LAUNCHER_OWNER');
  if (!owner.isNil() && app.respondsToSelector('yieldActivationToApplication:')) {
    const ra = $.NSRunningApplication.runningApplicationWithProcessIdentifier(parseInt(owner.js, 10));
    if (!ra.isNil()) app.yieldActivationToApplication(ra);
  }
  return 'play';
}
