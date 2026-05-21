// editor.jsx — Video Editor workspace: media bin · preview · inspector / above the timeline.

function VideoEditor() {
  const [tool, setTool] = useState('select');
  const [zoom, setZoom] = useState(60);   // px per second
  const [playing, setPlaying] = useState(false);
  const [playhead, setPlayhead] = useState(34);  // seconds
  const [selected, setSelected] = useState({ track: 0, clipId: 'c2' });
  const [snap, setSnap] = useState(true);
  const TIMELINE_LEN = 360; // seconds (6 min)

  // Drive playhead when playing
  useEffect(() => {
    if (!playing) return;
    const id = setInterval(() => setPlayhead(p => (p + 0.05) % TIMELINE_LEN), 50);
    return () => clearInterval(id);
  }, [playing]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
      {/* Top half: bin · preview · inspector */}
      <div style={{ display: 'flex', flex: '1 1 55%', minHeight: 0, borderBottom: '1px solid var(--border)' }}>
        <Splitter dir="h" initial={300} min={220} max={460} storageKey="ms-bin-w">
          <MediaBin/>
          <Splitter dir="h" initial={920} min={520} max={1400} storageKey="ms-prev-w">
            <PreviewMonitor playing={playing} setPlaying={setPlaying} playhead={playhead} setPlayhead={setPlayhead}/>
            <Inspector selected={selected}/>
          </Splitter>
        </Splitter>
      </div>

      {/* Toolbar */}
      <EditorToolbar
        tool={tool} setTool={setTool}
        zoom={zoom} setZoom={setZoom}
        snap={snap} setSnap={setSnap}
        playhead={playhead}
      />

      {/* Bottom half: timeline */}
      <div style={{ flex: '1 1 45%', minHeight: 0, display: 'flex' }}>
        <Timeline
          zoom={zoom} playhead={playhead} setPlayhead={setPlayhead}
          selected={selected} setSelected={setSelected}
          tool={tool} snap={snap}
        />
      </div>
    </div>
  );
}

// ─── Media Bin ───
const BIN = [
  { id: 'm1', kind: 'video', name: 'Spire — Ep 14 raw.mkv',     dur: '01:47:21', meta: '1080p60 · 14.2 GB', used: true },
  { id: 'm2', kind: 'video', name: 'Webcam α7C — Ep 14.mkv',     dur: '01:47:18', meta: '1080p60 · 4.8 GB',  used: true },
  { id: 'm3', kind: 'audio', name: 'Mic — SM7B (cleaned).wav',  dur: '01:47:08', meta: '48k · 1.1 GB',      used: true },
  { id: 'm4', kind: 'audio', name: 'Music — Synth bed.flac',     dur: '03:42:00', meta: '48k · 380 MB',      used: true },
  { id: 'm5', kind: 'video', name: 'Intro card v3.mp4',           dur: '00:00:08', meta: '4k60 · 92 MB',      used: true },
  { id: 'm6', kind: 'image', name: 'Lower-third — title.png',     dur: '—',        meta: '1920 × 240',         used: true },
  { id: 'm7', kind: 'video', name: 'B-roll — sunset.mp4',         dur: '00:00:32', meta: '4k30 · 220 MB',      used: false },
  { id: 'm8', kind: 'audio', name: 'SFX — whoosh 02.wav',         dur: '00:00:02', meta: '48k · 220 KB',      used: false },
  { id: 'm9', kind: 'image', name: 'Thumbnail — final.psd',       dur: '—',        meta: '1920 × 1080',        used: false, missing: true },
];

function MediaBin() {
  const [view, setView] = useState('list');
  const [q, setQ] = useState('');
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, flex: 1, height: '100%' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="media" size={14}/> Media Bin <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>9</span></div>
        <div className="panel-hd-tools">
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt="Import"><Icon name="upload" size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt={view==='grid'?'List':'Grid'} onClick={()=>setView(view==='grid'?'list':'grid')}><Icon name={view==='grid'?'list':'grid'} size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost"><Icon name="more" size={14}/></button>
        </div>
      </div>
      <div style={{ padding: '6px 8px', borderBottom: '1px solid var(--border)' }}>
        <div style={{ position: 'relative' }}>
          <Icon name="search" size={12} style={{ position: 'absolute', left: 8, top: '50%', transform: 'translateY(-50%)', color: 'var(--text-mute)' }}/>
          <input className="input" placeholder="Search bin · ⌘F" value={q} onChange={e=>setQ(e.target.value)} style={{ paddingLeft: 26 }}/>
        </div>
      </div>
      <div style={{ flex: 1, overflow: 'auto' }}>
        {view === 'list' ? <BinList items={BIN}/> : <BinGrid items={BIN}/>}
      </div>
      <div style={{ padding: '6px 10px', borderTop: '1px solid var(--border)', fontSize: 'var(--fs-xs)', color: 'var(--text-mute)', display: 'flex', justifyContent: 'space-between' }}>
        <span>6 used · 3 unused</span>
        <span>1 missing</span>
      </div>
    </div>
  );
}

function BinList({ items }) {
  return (
    <div style={{ padding: '4px 6px' }}>
      {items.map(m => (
        <div key={m.id} className="row" draggable style={{ height: 36, cursor: 'grab' }}>
          <span style={{ width: 24, height: 16, background: 'var(--surface-2)', borderRadius: 3, display: 'inline-flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-mute)', flex: '0 0 auto' }}>
            <Icon name={m.kind === 'video' ? 'editor' : m.kind === 'audio' ? 'speaker' : 'image'} size={10}/>
          </span>
          <div style={{ display: 'flex', flexDirection: 'column', minWidth: 0 }}>
            <span style={{ fontSize: 12, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: m.missing ? 'var(--warn)' : 'var(--text)' }}>{m.name}</span>
            <span className="mute mono" style={{ fontSize: 10 }}>{m.dur} · {m.meta}</span>
          </div>
          <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 4 }}>
            {m.missing && <span className="tag tag-warn">missing</span>}
            {!m.used && !m.missing && <span className="tag" style={{ fontSize: 9 }}>unused</span>}
          </div>
        </div>
      ))}
    </div>
  );
}

function BinGrid({ items }) {
  return (
    <div style={{ padding: 8, display: 'grid', gridTemplateColumns: 'repeat(2, 1fr)', gap: 8 }}>
      {items.map(m => (
        <div key={m.id} style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
          <Placeholder label={m.kind} ratio="16/9" radius={4} style={{ position: 'relative' }}/>
          <span style={{ fontSize: 11, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{m.name}</span>
        </div>
      ))}
    </div>
  );
}

// ─── Preview Monitor ───
function PreviewMonitor({ playing, setPlaying, playhead, setPlayhead }) {
  const fmt = (t) => {
    const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = Math.floor(t % 60), f = Math.floor((t % 1) * 60);
    return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}:${String(f).padStart(2,'0')}`;
  };
  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minWidth: 0, background: 'var(--bg-base)' }}>
      <div style={{ padding: 'var(--s-3) var(--s-4)', display: 'flex', alignItems: 'center', gap: 'var(--s-2)' }}>
        <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
          <button className="btn btn-sm" style={{ height: 22, background: 'var(--surface-2)' }}>Program</button>
          <button className="btn btn-sm btn-ghost" style={{ height: 22 }}>Source</button>
          <button className="btn btn-sm btn-ghost" style={{ height: 22 }}>Multicam</button>
        </div>
        <span className="mono mute" style={{ marginLeft: 'auto', fontSize: 'var(--fs-xs)' }}>1920 × 1080 · 60 fps</span>
      </div>
      <div style={{ flex: 1, minHeight: 0, padding: '0 var(--s-4)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
        <div style={{ position: 'relative', width: '100%', maxHeight: '100%', aspectRatio: '16/9' }}>
          <Placeholder label="Program · 00:34:08" ratio="16/9" radius={6} style={{ width: '100%', height: '100%' }}/>
          {/* Mock overlays */}
          <div style={{ position: 'absolute', right: '3%', bottom: '6%', width: '22%', aspectRatio: '16/9', border: '1.5px solid var(--accent)', borderRadius: 4, background: 'oklch(0 0 0 / 0.5)' }}/>
          <div style={{ position: 'absolute', left: '4%', top: '6%', padding: '4px 8px', background: 'oklch(0 0 0 / 0.55)', borderRadius: 4 }}><span className="mono" style={{ fontSize: 11 }}>"DODGE INTO RIPOSTE"</span></div>
        </div>
      </div>
      {/* Transport */}
      <div style={{ padding: 'var(--s-3) var(--s-4)', display: 'flex', alignItems: 'center', gap: 'var(--s-2)' }}>
        <button className="btn btn-icon btn-ghost tt-wrap" data-tt="Jump to start"><Icon name="skipBack" size={14}/></button>
        <button className="btn btn-icon btn-ghost tt-wrap" data-tt="-1 frame"><Icon name="chevLeft" size={14}/></button>
        <button className="btn btn-icon" onClick={() => setPlaying(p => !p)} style={{ background: 'var(--accent)', borderColor: 'var(--accent)', color: 'oklch(0.1 0 0)', width: 32, height: 32 }}>
          <Icon name={playing ? 'pause' : 'play'} size={14}/>
        </button>
        <button className="btn btn-icon btn-ghost tt-wrap" data-tt="+1 frame"><Icon name="chevRight" size={14}/></button>
        <button className="btn btn-icon btn-ghost tt-wrap" data-tt="Jump to end"><Icon name="skipFwd" size={14}/></button>

        <span className="mono" style={{ marginLeft: 12, fontSize: 'var(--fs-md)', fontWeight: 600 }}>{fmt(playhead)}</span>
        <span className="mono mute" style={{ marginLeft: 4, fontSize: 'var(--fs-xs)' }}>/ 00:06:00:00</span>

        <span style={{ marginLeft: 'auto', display: 'inline-flex', gap: 4 }}>
          <button className="btn btn-sm btn-ghost"><Icon name="scissors" size={11}/>Split <span className="kbd">⌘B</span></button>
          <button className="btn btn-sm btn-ghost"><Icon name="bell" size={11}/>Marker <span className="kbd">M</span></button>
          <button className="btn btn-sm"><Icon name="upload" size={11}/>Export</button>
        </span>
      </div>
    </div>
  );
}

// ─── Inspector ───
function Inspector({ selected }) {
  const [tab, setTab] = useState('effects');
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, height: '100%' }}>
      <div className="panel-hd">
        <div className="panel-hd-title">
          <Icon name="settings" size={14}/>
          <span>Inspector</span>
          <span className="tag tag-accent" style={{ marginLeft: 4 }}>V2 · Webcam clip</span>
        </div>
      </div>
      <div style={{ display: 'flex', gap: 0, padding: '6px 8px', borderBottom: '1px solid var(--border)' }}>
        {['effects', 'transform', 'audio', 'speed'].map(t => (
          <button key={t} onClick={() => setTab(t)} className="btn btn-sm btn-ghost" style={{
            height: 24, textTransform: 'capitalize',
            color: tab === t ? 'var(--text)' : 'var(--text-mute)',
            background: tab === t ? 'var(--surface-2)' : 'transparent',
          }}>{t}</button>
        ))}
      </div>
      <div style={{ padding: 'var(--s-3)', flex: 1, overflow: 'auto', display: 'flex', flexDirection: 'column', gap: 'var(--s-4)' }}>
        {tab === 'transform' && (
          <>
            <Field label="Position & Scale">
              <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6 }}>
                <Input label="X"    value="1488"/>
                <Input label="Y"    value="780"/>
                <Input label="Scale" value="100" suffix="%"/>
                <Input label="Rotation" value="0" suffix="°"/>
              </div>
            </Field>
            <Field label="Opacity">
              <SliderRow value={100} unit="%"/>
            </Field>
          </>
        )}
        {tab === 'effects' && (
          <>
            <EffectCard name="Color Correction" enabled>
              <SliderRow label="Exposure"   value={0.2} min={-1} max={1} unit=""/>
              <SliderRow label="Contrast"   value={8}    min={-100} max={100} unit=""/>
              <SliderRow label="Saturation" value={-4}   min={-100} max={100} unit=""/>
              <SliderRow label="Temperature" value={120} min={-1000} max={1000} unit="K"/>
            </EffectCard>
            <EffectCard name="Crop" enabled={false}/>
            <EffectCard name="Sharpen" enabled>
              <SliderRow label="Amount" value={28} min={0} max={100} unit="%"/>
            </EffectCard>
            <button className="btn btn-sm" style={{ alignSelf: 'flex-start' }}><Icon name="plus" size={11}/>Add Effect</button>
          </>
        )}
        {tab === 'audio' && (
          <>
            <Field label="Gain">
              <SliderRow value={-3} min={-30} max={12} unit="dB"/>
            </Field>
            <Field label="Pan">
              <SliderRow value={0} min={-100} max={100} unit=""/>
            </Field>
            <Field label="Channels">
              <select className="input">
                <option>Stereo · L+R</option>
                <option>Mono (Left only)</option>
                <option>Mono (Right only)</option>
              </select>
            </Field>
          </>
        )}
        {tab === 'speed' && (
          <>
            <Field label="Speed">
              <SliderRow value={100} min={10} max={400} unit="%"/>
            </Field>
            <Field label="Time remapping">
              <button className="btn btn-sm"><Icon name="plus" size={11}/>Add ramp keyframe</button>
            </Field>
          </>
        )}
      </div>
    </div>
  );
}

function EffectCard({ name, enabled = true, children }) {
  const [open, setOpen] = useState(true);
  const [en, setEn] = useState(enabled);
  return (
    <div style={{ background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '6px 8px', borderBottom: open && children ? '1px solid var(--border)' : 'none' }}>
        <input type="checkbox" checked={en} onChange={e => setEn(e.target.checked)} style={{ accentColor: 'var(--accent)' }}/>
        <span style={{ fontSize: 'var(--fs-sm)', fontWeight: 500, color: en ? 'var(--text)' : 'var(--text-mute)' }}>{name}</span>
        {children && (
          <button className="btn btn-sm btn-ghost" style={{ marginLeft: 'auto', padding: 2 }} onClick={() => setOpen(!open)}>
            <Icon name={open ? 'chevDown' : 'chevRight'} size={12}/>
          </button>
        )}
      </div>
      {open && children && <div style={{ padding: '8px 10px', display: 'flex', flexDirection: 'column', gap: 6 }}>{children}</div>}
    </div>
  );
}

function SliderRow({ label, value, min = 0, max = 100, unit = '' }) {
  const [v, setV] = useState(value);
  return (
    <div style={{ display: 'grid', gridTemplateColumns: label ? '100px 1fr 56px' : '1fr 56px', alignItems: 'center', gap: 8 }}>
      {label && <span style={{ fontSize: 11, color: 'var(--text-mute)' }}>{label}</span>}
      <input type="range" min={min} max={max} value={v} onChange={e => setV(+e.target.value)} style={{ accentColor: 'var(--accent)' }}/>
      <span className="mono mute" style={{ fontSize: 10, textAlign: 'right' }}>{v}{unit}</span>
    </div>
  );
}

// ─── Editor Toolbar ───
function EditorToolbar({ tool, setTool, zoom, setZoom, snap, setSnap, playhead }) {
  const tools = [
    { id: 'select', icon: 'drag',    label: 'Select · V' },
    { id: 'razor',  icon: 'scissors', label: 'Razor · C' },
    { id: 'slip',   icon: 'refresh', label: 'Slip · Y' },
    { id: 'hand',   icon: 'layers',  label: 'Hand · H' },
  ];
  return (
    <div style={{ flex: '0 0 auto', height: 36, padding: '0 var(--s-4)', display: 'flex', alignItems: 'center', gap: 'var(--s-3)', background: 'var(--surface-0)', borderTop: '1px solid var(--border)' }}>
      <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
        {tools.map(t => (
          <button key={t.id} className="tt-wrap" data-tt={t.label} onClick={() => setTool(t.id)} style={{
            width: 24, height: 22, borderRadius: 3,
            background: tool === t.id ? 'var(--surface-2)' : 'transparent',
            display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
            color: tool === t.id ? 'var(--text)' : 'var(--text-mute)',
          }}>
            <Icon name={t.icon} size={12}/>
          </button>
        ))}
      </div>
      <div style={{ width: 1, height: 18, background: 'var(--border)' }}/>
      <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Undo  ⌘Z"><Icon name="refresh" size={12} style={{ transform: 'scaleX(-1)' }}/></button>
      <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Redo  ⌘⇧Z"><Icon name="refresh" size={12}/></button>
      <button className="btn btn-sm btn-ghost" onClick={() => setSnap(!snap)} style={{ color: snap ? 'var(--accent-hi)' : 'var(--text-mute)' }}>
        <Icon name="check" size={11}/>Snap
      </button>

      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 8 }}>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>Zoom</span>
        <input type="range" min="10" max="200" value={zoom} onChange={e => setZoom(+e.target.value)} style={{ width: 140, accentColor: 'var(--accent)' }}/>
        <span className="mono mute" style={{ fontSize: 10, minWidth: 40 }}>{zoom}px/s</span>
        <button className="btn btn-sm btn-ghost"><Icon name="zoomIn" size={12}/></button>
      </div>

      <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
        <span className="dot-ok"/>
        <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>Autosaved · 14s ago</span>
      </div>
    </div>
  );
}

// ─── TIMELINE ───
// Tracks
const TRACKS = [
  { id: 'v3', kind: 'video', name: 'V3 — Titles' },
  { id: 'v2', kind: 'video', name: 'V2 — Webcam' },
  { id: 'v1', kind: 'video', name: 'V1 — Gameplay' },
  { id: 'a1', kind: 'audio', name: 'A1 — Mic' },
  { id: 'a2', kind: 'audio', name: 'A2 — Game Audio' },
  { id: 'a3', kind: 'audio', name: 'A3 — Music Bed' },
];

// Clips on the timeline: {trackId, start, dur, color, label, isAudio}
const CLIPS = [
  // V3 — titles
  { id: 'c-title-1', trackId: 'v3', start: 0,   dur: 8,  color: 'oklch(0.62 0.16 305)', label: 'Intro card', tag: 'IMG' },
  { id: 'c-title-2', trackId: 'v3', start: 60,  dur: 4,  color: 'oklch(0.62 0.16 305)', label: 'Lower-third · No-hit', tag: 'IMG' },
  { id: 'c-title-3', trackId: 'v3', start: 180, dur: 4,  color: 'oklch(0.62 0.16 305)', label: 'Lower-third · Dodge', tag: 'IMG' },

  // V2 — webcam
  { id: 'c2',  trackId: 'v2', start: 8,   dur: 120, color: 'oklch(0.62 0.14 220)', label: 'Webcam α7C', tag: 'VID' },
  { id: 'c2b', trackId: 'v2', start: 140, dur: 96,  color: 'oklch(0.62 0.14 220)', label: 'Webcam · phase 3 reactions', tag: 'VID' },

  // V1 — gameplay
  { id: 'c1',  trackId: 'v1', start: 0,   dur: 8,   color: 'oklch(0.55 0.10 220)', label: 'Intro card · BG', tag: 'VID' },
  { id: 'c1b', trackId: 'v1', start: 8,   dur: 132, color: 'oklch(0.60 0.12 220)', label: 'Spire — phase 1', tag: 'VID' },
  { id: 'c1c', trackId: 'v1', start: 140, dur: 60,  color: 'oklch(0.60 0.12 220)', label: 'Spire — phase 2', tag: 'VID' },
  { id: 'c1d', trackId: 'v1', start: 200, dur: 88,  color: 'oklch(0.60 0.12 220)', label: 'Spire — phase 3', tag: 'VID' },

  // A1 — mic
  { id: 'a-mic',  trackId: 'a1', start: 8,  dur: 280, color: 'oklch(0.66 0.14 145)', label: 'Mic — SM7B', tag: 'AUD', isAudio: true },

  // A2 — game audio
  { id: 'a-game', trackId: 'a2', start: 8,  dur: 280, color: 'oklch(0.62 0.10 145)', label: 'Game audio', tag: 'AUD', isAudio: true },

  // A3 — music bed
  { id: 'a-music-1', trackId: 'a3', start: 0,   dur: 64,  color: 'oklch(0.70 0.12 75)', label: 'Music · Synth bed', tag: 'AUD', isAudio: true },
  { id: 'a-music-2', trackId: 'a3', start: 200, dur: 90,  color: 'oklch(0.70 0.12 75)', label: 'Music · Outro',     tag: 'AUD', isAudio: true },
];

const MARKERS = [
  { t: 60,  label: 'No-hit', color: 'var(--accent)' },
  { t: 140, label: 'Phase 2', color: 'var(--warn)' },
  { t: 200, label: 'Boss dies', color: 'var(--rec)' },
];

function Timeline({ zoom, playhead, setPlayhead, selected, setSelected, tool, snap }) {
  const TIMELINE_LEN = 360;
  const scrollRef = useRef(null);
  const headerH = 26;
  const trackH = 44;
  const trackHHeader = 30;
  const headWidth = 200;
  const fullWidth = TIMELINE_LEN * zoom;

  // Drag to scrub the playhead via ruler
  const [scrubbing, setScrubbing] = useState(false);
  const rulerRef = useRef(null);

  useEffect(() => {
    if (!scrubbing) return;
    const onMove = (e) => {
      const r = rulerRef.current?.getBoundingClientRect(); if (!r) return;
      const px = (e.clientX - r.left) + (scrollRef.current?.scrollLeft || 0);
      const t = Math.max(0, Math.min(TIMELINE_LEN, px / zoom));
      setPlayhead(snap ? Math.round(t * 4) / 4 : t);
    };
    const onUp = () => setScrubbing(false);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return () => { window.removeEventListener('mousemove', onMove); window.removeEventListener('mouseup', onUp); };
  }, [scrubbing, zoom, snap, setPlayhead]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0, background: 'var(--surface-0)' }}>
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>
        {/* Track headers (fixed left) */}
        <div style={{ width: headWidth, flex: '0 0 auto', borderRight: '1px solid var(--border)', background: 'var(--surface-0)' }}>
          <div style={{ height: headerH, borderBottom: '1px solid var(--border)', background: 'var(--surface-1)' }}/>
          {TRACKS.map(tr => (
            <TrackHeader key={tr.id} track={tr} height={trackH}/>
          ))}
        </div>

        {/* Scrollable timeline canvas */}
        <div ref={scrollRef} style={{ flex: 1, minWidth: 0, overflow: 'auto', position: 'relative' }}>
          <div style={{ width: fullWidth, minHeight: '100%', position: 'relative' }}>
            {/* Ruler */}
            <div ref={rulerRef} onMouseDown={() => setScrubbing(true)} style={{
              height: headerH, background: 'var(--surface-1)', borderBottom: '1px solid var(--border)',
              position: 'sticky', top: 0, zIndex: 4, cursor: 'col-resize',
            }}>
              <Ruler totalSec={TIMELINE_LEN} zoom={zoom}/>
              {/* Markers */}
              {MARKERS.map(m => (
                <div key={m.t} style={{ position: 'absolute', top: 0, bottom: 0, left: m.t * zoom, display: 'flex', alignItems: 'flex-end' }}>
                  <div style={{ width: 0, borderLeft: '6px solid transparent', borderRight: '6px solid transparent', borderBottom: `8px solid ${m.color}` }}/>
                </div>
              ))}
            </div>

            {/* Tracks */}
            {TRACKS.map(tr => (
              <TrackRow key={tr.id} track={tr} height={trackH} zoom={zoom}
                clips={CLIPS.filter(c => c.trackId === tr.id)}
                selected={selected} onSelect={setSelected}
                tool={tool}
              />
            ))}

            {/* Playhead */}
            <div style={{
              position: 'absolute', top: 0, bottom: 0, left: playhead * zoom,
              width: 1, background: 'var(--rec-hi)', pointerEvents: 'none', zIndex: 6,
            }}>
              <div style={{ position: 'absolute', top: 0, left: -6, width: 12, height: headerH, background: 'var(--rec)', clipPath: 'polygon(0 0, 100% 0, 100% 60%, 50% 100%, 0 60%)' }}/>
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}

function Ruler({ totalSec, zoom }) {
  // Decide tick interval based on zoom
  const major = zoom > 100 ? 1 : zoom > 50 ? 2 : zoom > 25 ? 5 : 10;
  const ticks = [];
  for (let s = 0; s <= totalSec; s += major) {
    const m = Math.floor(s / 60); const ss = s % 60;
    ticks.push({ s, label: `${m}:${String(ss).padStart(2,'0')}` });
  }
  return (
    <>
      {ticks.map(t => (
        <div key={t.s} style={{ position: 'absolute', top: 0, bottom: 0, left: t.s * zoom, borderLeft: '1px solid var(--border)' }}>
          <span className="mono mute" style={{ position: 'absolute', top: 4, left: 4, fontSize: 10 }}>{t.label}</span>
        </div>
      ))}
    </>
  );
}

function TrackHeader({ track, height }) {
  const isAudio = track.kind === 'audio';
  return (
    <div style={{ height, display: 'flex', alignItems: 'center', gap: 6, padding: '0 8px', borderBottom: '1px solid var(--border)', background: 'var(--surface-0)' }}>
      <span style={{ width: 18, fontSize: 10, color: isAudio ? 'var(--meter-green)' : 'var(--accent-hi)', fontFamily: 'var(--font-mono)', fontWeight: 600 }}>{track.id.toUpperCase()}</span>
      <span style={{ fontSize: 11, color: 'var(--text-dim)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{track.name.split('—')[1]?.trim() || track.name}</span>
      <div style={{ marginLeft: 'auto', display: 'inline-flex', gap: 2 }}>
        <button className="btn-ghost tt-wrap" data-tt="Mute" style={{ padding: 3, color: 'var(--text-mute)' }}><Icon name={isAudio ? 'speaker' : 'eye'} size={11}/></button>
        <button className="btn-ghost tt-wrap" data-tt="Solo" style={{ padding: 3, color: 'var(--text-mute)' }}><span style={{ fontFamily: 'var(--font-mono)', fontSize: 9, fontWeight: 600 }}>S</span></button>
        <button className="btn-ghost tt-wrap" data-tt="Lock" style={{ padding: 3, color: 'var(--text-mute)' }}><Icon name="unlock" size={11}/></button>
      </div>
    </div>
  );
}

function TrackRow({ track, height, zoom, clips, selected, onSelect, tool }) {
  return (
    <div style={{ height, position: 'relative', borderBottom: '1px solid var(--border)', background: track.kind === 'audio' ? 'oklch(0.20 0.006 270)' : 'var(--surface-0)' }}>
      {/* gridlines every 10s */}
      {Array.from({ length: 36 }).map((_, i) => (
        <div key={i} style={{ position: 'absolute', top: 0, bottom: 0, left: i * 10 * zoom, borderLeft: i % 6 === 0 ? '1px solid oklch(0.27 0.006 270)' : '1px dashed oklch(0.22 0.006 270)' }}/>
      ))}
      {clips.map(c => (
        <Clip key={c.id} clip={c} zoom={zoom} height={height}
              active={selected?.clipId === c.id}
              onClick={() => onSelect({ track: track.id, clipId: c.id })}/>
      ))}
    </div>
  );
}

function Clip({ clip, zoom, height, active, onClick }) {
  const left = clip.start * zoom;
  const width = clip.dur * zoom;
  const isAudio = clip.isAudio;
  return (
    <div
      onClick={onClick}
      style={{
        position: 'absolute', top: 3, left, width, height: height - 6,
        background: clip.color,
        border: active ? '1.5px solid oklch(0.97 0 0)' : '1px solid oklch(0 0 0 / 0.3)',
        borderRadius: 3, padding: '3px 6px',
        color: 'oklch(0.1 0 0)',
        display: 'flex', flexDirection: 'column', justifyContent: 'space-between',
        overflow: 'hidden', cursor: 'pointer',
        boxShadow: active ? '0 0 0 1px var(--accent)' : 'none',
      }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 6, fontSize: 9, fontWeight: 600, opacity: 0.85 }}>
        <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{clip.label}</span>
        <span className="mono" style={{ flex: '0 0 auto' }}>{clip.tag}</span>
      </div>
      {/* Audio waveform mock */}
      {isAudio && <Waveform width={width - 12}/>}
    </div>
  );
}

function Waveform({ width }) {
  const bars = Math.max(20, Math.floor(width / 3));
  // Deterministic pseudo-random pattern
  return (
    <div style={{ display: 'flex', alignItems: 'center', height: 14, gap: 1 }}>
      {Array.from({ length: bars }).map((_, i) => {
        const h = 2 + Math.abs(Math.sin(i * 0.7) * 0.7 + Math.sin(i * 0.13) * 0.4) * 10;
        return <div key={i} style={{ width: 2, height: h, background: 'oklch(0 0 0 / 0.55)', borderRadius: 1 }}/>;
      })}
    </div>
  );
}

Object.assign(window, { VideoEditor });
