// primitives.jsx — shared atoms: icons, splitter, vu meter, dropdown, simple ui
// Each component is window-exported at the bottom so other JSX scripts can use them.

const { useState, useEffect, useRef, useCallback, useMemo, createContext, useContext } = React;

// ─────────────────────────────────────────────────────────────────────────────
// Icon set — geometric, stroke-only. Tiny + readable at 14–18px.
// ─────────────────────────────────────────────────────────────────────────────
const ICONS = {
  dashboard: <><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="4" rx="1.5"/><rect x="14" y="10" width="7" height="11" rx="1.5"/><rect x="3" y="13" width="7" height="8" rx="1.5"/></>,
  record:    <><circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3.5" fill="currentColor" stroke="none"/></>,
  stream:    <><path d="M5 7c0 4 4 6 7 6s7-2 7-6"/><path d="M5 17c0-4 4-6 7-6s7 2 7 6"/></>,
  editor:    <><rect x="3" y="6" width="18" height="4" rx="1"/><rect x="3" y="14" width="18" height="4" rx="1"/><circle cx="8" cy="8" r="1" fill="currentColor"/><circle cx="16" cy="16" r="1" fill="currentColor"/></>,
  clips:     <><rect x="3" y="5" width="13" height="14" rx="1.5"/><path d="M16 9l5-3v12l-5-3"/></>,
  media:     <><rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 15l5-4 4 3 3-2 6 5"/></>,
  projects:  <><path d="M3 6.5C3 5.7 3.7 5 4.5 5h4l2 2h9c.8 0 1.5.7 1.5 1.5v8c0 .8-.7 1.5-1.5 1.5h-15c-.8 0-1.5-.7-1.5-1.5V6.5z"/></>,
  render:    <><rect x="3" y="4" width="18" height="6" rx="1.5"/><rect x="3" y="14" width="18" height="6" rx="1.5"/><circle cx="7" cy="7"  r="1" fill="currentColor"/><circle cx="7" cy="17" r="1" fill="currentColor"/></>,
  ai:        <><path d="M12 3l2.5 5 5.5.8-4 3.9.9 5.5L12 15.6 7.1 18.2 8 12.7 4 8.8l5.5-.8z"/></>,
  settings:  <><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></>,
  plus:      <><path d="M12 5v14M5 12h14"/></>,
  minus:     <><path d="M5 12h14"/></>,
  search:    <><circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3"/></>,
  filter:    <><path d="M4 5h16l-6 8v6l-4-2v-4z"/></>,
  grid:      <><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/></>,
  list:      <><path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/></>,
  more:      <><circle cx="5" cy="12" r="1" fill="currentColor"/><circle cx="12" cy="12" r="1" fill="currentColor"/><circle cx="19" cy="12" r="1" fill="currentColor"/></>,
  play:      <><path d="M7 5v14l12-7z" fill="currentColor"/></>,
  pause:     <><rect x="6" y="5" width="4" height="14" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" fill="currentColor" stroke="none"/></>,
  stop:      <><rect x="6" y="6" width="12" height="12" fill="currentColor" stroke="none" rx="1"/></>,
  skipFwd:   <><path d="M5 5v14l10-7zM17 5v14"/></>,
  skipBack:  <><path d="M19 5v14L9 12zM7 5v14"/></>,
  upload:    <><path d="M12 16V4M5 11l7-7 7 7M4 20h16"/></>,
  download:  <><path d="M12 4v12M5 11l7 7 7-7M4 20h16"/></>,
  trash:     <><path d="M4 7h16M9 7V5a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12"/></>,
  star:      <><path d="M12 4l2.5 5 5.5.8-4 3.9.9 5.5L12 16.6 7.1 19.2 8 13.7 4 9.8l5.5-.8z"/></>,
  folder:    <><path d="M3 6.5C3 5.7 3.7 5 4.5 5h4l2 2h9c.8 0 1.5.7 1.5 1.5v8c0 .8-.7 1.5-1.5 1.5h-15c-.8 0-1.5-.7-1.5-1.5V6.5z"/></>,
  cut:       <><circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M8 8l12 8M8 16l12-8"/></>,
  layers:    <><path d="M12 4l9 4.5-9 4.5-9-4.5zM3 13l9 4.5 9-4.5M3 17.5l9 4.5 9-4.5"/></>,
  mic:       <><rect x="9" y="3" width="6" height="12" rx="3"/><path d="M5 11a7 7 0 0 0 14 0M12 18v3M9 21h6"/></>,
  micOff:    <><path d="M9 3.5a3 3 0 0 1 6 0v6M9 9v.5a3 3 0 0 0 4.7 2.5M5 11a7 7 0 0 0 11.5 5.3M12 18v3M9 21h6M3 3l18 18"/></>,
  camera:    <><rect x="3" y="6" width="13" height="12" rx="1.5"/><path d="M16 10l5-3v10l-5-3z"/></>,
  display:   <><rect x="3" y="4" width="18" height="12" rx="1.5"/><path d="M9 20h6M12 16v4"/></>,
  window:    <><rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 9h18"/></>,
  text:      <><path d="M5 5h14M12 5v14M9 19h6"/></>,
  image:     <><rect x="3" y="5" width="18" height="14" rx="1.5"/><circle cx="9" cy="10" r="1.5"/><path d="M21 15l-5-4-4 3-3-2-6 5"/></>,
  browser:   <><rect x="3" y="5" width="18" height="14" rx="1.5"/><path d="M3 9h18M6.5 7h.01M9 7h.01"/></>,
  speaker:   <><path d="M5 10v4h3l5 4V6L8 10zM17 8a5 5 0 0 1 0 8M19.5 5a9 9 0 0 1 0 14"/></>,
  eye:       <><path d="M2 12s4-7 10-7 10 7 10 7-4 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="2.5"/></>,
  eyeOff:    <><path d="M3 3l18 18M10 6c.6-.1 1.3-.2 2-.2 6 0 10 6.2 10 6.2a16 16 0 0 1-2.6 3.2M6.6 6.8C4 8.6 2 12 2 12s4 7 10 7c2 0 3.7-.7 5-1.7"/></>,
  lock:      <><rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></>,
  unlock:    <><rect x="5" y="11" width="14" height="9" rx="1.5"/><path d="M8 11V7a4 4 0 0 1 7.5-2"/></>,
  chevDown:  <><path d="M6 9l6 6 6-6"/></>,
  chevRight: <><path d="M9 6l6 6-6 6"/></>,
  chevLeft:  <><path d="M15 6l-6 6 6 6"/></>,
  close:     <><path d="M6 6l12 12M18 6L6 18"/></>,
  check:     <><path d="M5 12l5 5 9-11"/></>,
  alert:     <><path d="M12 3l10 18H2z"/><path d="M12 10v5M12 18v.01"/></>,
  info:      <><circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 8v.01"/></>,
  bell:      <><path d="M6 9a6 6 0 0 1 12 0c0 5 2 6 2 6H4s2-1 2-6zM10 19a2 2 0 0 0 4 0"/></>,
  cpu:       <><rect x="6" y="6" width="12" height="12" rx="1.5"/><rect x="9" y="9" width="6" height="6" rx="0.5"/><path d="M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"/></>,
  disk:      <><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/></>,
  link:      <><path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 1 0-5.7-5.7l-1 1M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 1 0 5.7 5.7l1-1"/></>,
  scissors:  <><circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M8 8l12 8M8 16l12-8"/></>,
  drag:      <><circle cx="9" cy="6"  r="1" fill="currentColor"/><circle cx="15" cy="6"  r="1" fill="currentColor"/><circle cx="9" cy="12" r="1" fill="currentColor"/><circle cx="15" cy="12" r="1" fill="currentColor"/><circle cx="9" cy="18" r="1" fill="currentColor"/><circle cx="15" cy="18" r="1" fill="currentColor"/></>,
  command:   <><path d="M9 6a3 3 0 1 0 0 6h6a3 3 0 1 0 0-6 3 3 0 0 0-3 3v6a3 3 0 1 1-3 3 3 3 0 0 1 3-3h6a3 3 0 1 1-3 3"/></>,
  zoomIn:    <><circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3M11 8v6M8 11h6"/></>,
  zoomOut:   <><circle cx="11" cy="11" r="6.5"/><path d="M20 20l-4.3-4.3M8 11h6"/></>,
  refresh:   <><path d="M21 12a9 9 0 1 1-3-6.7L21 8M21 3v5h-5"/></>,
  bolt:      <><path d="M13 3L4 14h7l-1 7 9-11h-7z"/></>,
  sparkle:   <><path d="M12 4v6M12 14v6M4 12h6M14 12h6M6 6l3 3M15 15l3 3M18 6l-3 3M9 15l-3 3"/></>,
  question:  <><circle cx="12" cy="12" r="9"/><path d="M9.5 9a2.5 2.5 0 1 1 3.5 2.3c-.7.4-1 1-1 1.7M12 17v.01"/></>,
};

function Icon({ name, size = 16, stroke = 1.5, color, style, ...rest }) {
  const node = ICONS[name];
  if (!node) return null;
  return (
    <svg width={size} height={size} viewBox="0 0 24 24"
         fill="none" stroke={color || "currentColor"} strokeWidth={stroke}
         strokeLinecap="round" strokeLinejoin="round"
         style={{ flex: '0 0 auto', ...style }} {...rest}>
      {node}
    </svg>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Splitter — generic resizable two-pane container.
//   <Splitter dir="h" initial={280} min={180} max={500}>
//     <LeftPane/>
//     <RightPane/>
//   </Splitter>
// ─────────────────────────────────────────────────────────────────────────────
function Splitter({ dir = 'h', initial = 280, min = 120, max = 800, children, storageKey }) {
  const isH = dir === 'h';
  const [size, setSize] = useState(() => {
    if (storageKey) {
      try { const v = +localStorage.getItem(storageKey); if (v >= min && v <= max) return v; } catch (e) {}
    }
    return initial;
  });
  const [drag, setDrag] = useState(false);
  const wrapRef = useRef(null);

  useEffect(() => {
    if (!drag) return;
    const onMove = (e) => {
      const r = wrapRef.current?.getBoundingClientRect(); if (!r) return;
      const next = isH ? (e.clientX - r.left) : (e.clientY - r.top);
      const clamped = Math.max(min, Math.min(max, next));
      setSize(clamped);
      if (storageKey) try { localStorage.setItem(storageKey, String(clamped)); } catch (e) {}
    };
    const onUp = () => setDrag(false);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    document.body.style.cursor = isH ? 'col-resize' : 'row-resize';
    document.body.style.userSelect = 'none';
    return () => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      document.body.style.cursor = '';
      document.body.style.userSelect = '';
    };
  }, [drag, isH, min, max, storageKey]);

  const kids = React.Children.toArray(children);
  return (
    <div ref={wrapRef} style={{ display: 'flex', flexDirection: isH ? 'row' : 'column', flex: '1 1 auto', minWidth: 0, minHeight: 0, width: '100%', height: '100%' }}>
      <div style={{ flex: `0 0 ${size}px`, minWidth: 0, minHeight: 0, display: 'flex' }}>{kids[0]}</div>
      <div className={isH ? 'split-h' : 'split-v'}
           data-dragging={drag}
           onMouseDown={() => setDrag(true)} />
      <div style={{ flex: '1 1 auto', minWidth: 0, minHeight: 0, display: 'flex' }}>{kids[1]}</div>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// VuMeter — stereo, animated bar with peak hold.
// ─────────────────────────────────────────────────────────────────────────────
function VuMeter({ level = 0.4, peak = 0.55, height = 6, width = '100%' }) {
  const seg = (pct, color) => `${color} 0 ${pct}%`;
  // Map 0..1 to colored gradient. Yellow at .65, red at .85.
  const pct = Math.max(0, Math.min(1, level)) * 100;
  const peakPct = Math.max(0, Math.min(1, peak)) * 100;
  return (
    <div style={{ position: 'relative', width, height, background: 'var(--surface-2)', borderRadius: 2, overflow: 'hidden' }}>
      <div style={{
        position: 'absolute', inset: 0, width: `${pct}%`,
        background: `linear-gradient(to right,
          var(--meter-green) 0%, var(--meter-green) 60%,
          var(--meter-yellow) 75%, var(--meter-yellow) 85%,
          var(--meter-red) 92%, var(--meter-red) 100%)`,
        transition: 'width 60ms linear',
      }}/>
      <div style={{
        position: 'absolute', top: 0, bottom: 0, left: `${peakPct}%`,
        width: 1.5, background: 'oklch(0.98 0 0 / 0.85)',
        transition: 'left 200ms ease-out',
      }}/>
      {/* Tick marks */}
      {[20, 40, 60, 80].map(t => (
        <div key={t} style={{ position: 'absolute', top: 0, bottom: 0, left: `${t}%`, width: 1, background: 'oklch(0 0 0 / 0.35)' }}/>
      ))}
    </div>
  );
}

// Hook: drive a random-walking level for an audio-meter-looking animation.
function useFakeLevel(active = true, base = 0.35, variance = 0.22) {
  const [lvl, setLvl] = useState(base);
  const [peak, setPeak] = useState(base);
  const peakRef = useRef(base);
  const peakAt = useRef(Date.now());
  useEffect(() => {
    if (!active) { setLvl(0); setPeak(0); return; }
    let raf;
    const tick = () => {
      const target = Math.max(0.05, Math.min(0.95, base + (Math.random() - 0.5) * variance * 2));
      setLvl(prev => prev + (target - prev) * 0.35);
      if (target > peakRef.current) { peakRef.current = target; peakAt.current = Date.now(); }
      else if (Date.now() - peakAt.current > 600) { peakRef.current = Math.max(target, peakRef.current - 0.005); }
      setPeak(peakRef.current);
      raf = setTimeout(tick, 60);
    };
    tick();
    return () => clearTimeout(raf);
  }, [active, base, variance]);
  return { level: lvl, peak };
}

// Hook: count up timer when running.
function useTimer(running) {
  const [s, setS] = useState(0);
  useEffect(() => {
    if (!running) return;
    const id = setInterval(() => setS(x => x + 1), 1000);
    return () => clearInterval(id);
  }, [running]);
  useEffect(() => { if (!running) setS(0); }, [running]);
  const fmt = (n) => {
    const h = Math.floor(n / 3600), m = Math.floor((n % 3600) / 60), sec = n % 60;
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
  };
  return { seconds: s, text: fmt(s) };
}

// ─────────────────────────────────────────────────────────────────────────────
// SimStateContext — global "simulated app state" used by Tweaks
//   { mode: 'idle' | 'recording' | 'streaming' | 'rendering', workspace, showAI, ... }
// ─────────────────────────────────────────────────────────────────────────────
const SimStateContext = createContext(null);
function useSim() { return useContext(SimStateContext); }

// ─────────────────────────────────────────────────────────────────────────────
// Stat — small "label : value" readout used in headers / status bars
// ─────────────────────────────────────────────────────────────────────────────
function Stat({ label, value, tone, mono = true }) {
  const color = tone === 'rec'  ? 'var(--rec-hi)'
              : tone === 'live' ? 'var(--rec-hi)'
              : tone === 'ok'   ? 'var(--success)'
              : tone === 'warn' ? 'var(--warn)'
              : tone === 'accent' ? 'var(--accent-hi)'
              : 'var(--text)';
  return (
    <div style={{ display: 'inline-flex', alignItems: 'baseline', gap: 6, fontSize: 'var(--fs-xs)' }}>
      <span style={{ color: 'var(--text-mute)', textTransform: 'uppercase', letterSpacing: '0.08em', fontWeight: 500 }}>{label}</span>
      <span className={mono ? 'mono' : ''} style={{ color, fontWeight: 600 }}>{value}</span>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Section header (inside cards/panels)
// ─────────────────────────────────────────────────────────────────────────────
function SectionH({ children, right }) {
  return (
    <div className="section-h">
      <span>{children}</span>
      {right && <span style={{ marginLeft: 'auto', textTransform: 'none', letterSpacing: 0, fontWeight: 400, color: 'var(--text-mute)' }}>{right}</span>}
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Striped placeholder thumb — used in lieu of real imagery
// ─────────────────────────────────────────────────────────────────────────────
function Placeholder({ label, ratio = '16/9', radius = 6, mono = true, style }) {
  return (
    <div className="placeholder-fill" style={{
      aspectRatio: ratio,
      borderRadius: radius,
      border: '1px solid var(--border)',
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      color: 'var(--text-mute)', fontFamily: mono ? 'var(--font-mono)' : 'inherit',
      fontSize: 'var(--fs-xs)', letterSpacing: '0.06em', textTransform: 'uppercase',
      overflow: 'hidden',
      ...style,
    }}>
      {label}
    </div>
  );
}

Object.assign(window, {
  Icon, ICONS, Splitter, VuMeter, useFakeLevel, useTimer,
  SimStateContext, useSim, Stat, SectionH, Placeholder,
});
