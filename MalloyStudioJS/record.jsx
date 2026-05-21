// record.jsx — Recording Studio screen: preview canvas + scenes + sources + audio mixer + controls.

function RecordingStudio() {
  const sim = useSim();
  const isRec = sim.mode === 'recording';
  const rec = useTimer(isRec);
  const [activeScene, setActiveScene] = useState('gameplay');
  const [selSource, setSelSource] = useState('cam');
  const [transition, setTransition] = useState('cut');
  const [profile, setProfile] = useState('NVENC · 1080p60 · 8 Mb/s');

  // Audio
  const mic = useFakeLevel(true, 0.45, 0.22);
  const desktop = useFakeLevel(true, 0.55, 0.18);
  const music = useFakeLevel(true, 0.30, 0.15);
  const cam = useFakeLevel(true, 0.20, 0.10);

  return (
    <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
      {/* Left: scenes + sources */}
      <div style={{ width: 280, borderRight: '1px solid var(--border)', background: 'var(--surface-0)', display: 'flex', flexDirection: 'column', flex: '0 0 auto' }}>
        <ScenesPanel active={activeScene} onPick={setActiveScene} />
        <SourcesPanel active={activeScene} selected={selSource} onSelect={setSelSource} />
      </div>

      {/* Center: preview + controls */}
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <PreviewArea sim={sim} activeScene={activeScene} />
        <ControlsBar
          sim={sim} rec={rec}
          transition={transition} setTransition={setTransition}
          profile={profile} setProfile={setProfile}
        />
      </div>

      {/* Right: mixer + inspector */}
      <div style={{ width: 360, borderLeft: '1px solid var(--border)', background: 'var(--surface-0)', display: 'flex', flexDirection: 'column', flex: '0 0 auto' }}>
        <AudioMixer channels={[
          { name: 'Microphone — SM7B', sub: 'USB · gain +12 dB',  ...mic,     muted: false },
          { name: 'Desktop Audio',     sub: 'WASAPI loopback',     ...desktop, muted: false },
          { name: 'Music Bed',         sub: 'Browser source',      ...music,   muted: false },
          { name: 'Cam Mic',           sub: 'α7C internal · -18 dB', ...cam,   muted: true },
        ]}/>
        <SourceInspector selected={selSource} />
      </div>
    </div>
  );
}

// ─── Scenes ───
const SCENES = [
  { id: 'starting',  label: 'Starting Soon',   hotkey: 'Ctrl+1' },
  { id: 'gameplay',  label: 'Gameplay',        hotkey: 'Ctrl+2' },
  { id: 'brb',       label: 'Be Right Back',   hotkey: 'Ctrl+3' },
  { id: 'webcam',    label: 'Webcam Full',     hotkey: 'Ctrl+4' },
  { id: 'ending',    label: 'Ending',          hotkey: 'Ctrl+5' },
];

function ScenesPanel({ active, onPick }) {
  return (
    <div className="panel" style={{ border: 0, borderBottom: '1px solid var(--border)', borderRadius: 0, flex: '0 0 auto' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="layers" size={14}/> Scenes</div>
        <div className="panel-hd-tools">
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt="Add scene"><Icon name="plus" size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt="Remove"><Icon name="minus" size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost"><Icon name="more" size={14}/></button>
        </div>
      </div>
      <div style={{ padding: '4px 6px', maxHeight: 200, overflow: 'auto' }}>
        {SCENES.map(s => (
          <div key={s.id} className="row" data-active={active === s.id} onClick={() => onPick(s.id)} style={{ cursor: 'pointer' }}>
            <Icon name={active === s.id ? 'play' : 'layers'} size={12} style={{ color: active === s.id ? 'var(--accent)' : 'var(--text-mute)' }}/>
            <span style={{ fontSize: 'var(--fs-sm)' }}>{s.label}</span>
            <span className="row-meta">{s.hotkey}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── Sources (per active scene) ───
const SOURCES_BY_SCENE = {
  gameplay: [
    { id: 'game',  type: 'window',  name: 'Spire of the Hollow Sun', visible: true,  locked: false },
    { id: 'cam',   type: 'camera',  name: 'Webcam — α7C',            visible: true,  locked: false },
    { id: 'mic',   type: 'mic',     name: 'Mic — SM7B',              visible: true,  locked: false, audio: true },
    { id: 'over',  type: 'image',   name: 'Overlay — Bottom Bar',    visible: true,  locked: true },
    { id: 'alerts',type: 'browser', name: 'Alerts — Streamlabs',     visible: true,  locked: false },
    { id: 'chat',  type: 'browser', name: 'Chat — Twitch',           visible: false, locked: false },
    { id: 'title', type: 'text',    name: '"NO-HIT ATTEMPT"',        visible: true,  locked: false },
  ],
  starting: [
    { id: 'bg',    type: 'image',   name: 'BG — Loop',                 visible: true, locked: false },
    { id: 'count', type: 'browser', name: 'Countdown 5:00',            visible: true, locked: false },
    { id: 'mic',   type: 'mic',     name: 'Mic — SM7B (low)',          visible: true, locked: false, audio: true },
  ],
  brb:    [{ id: 'bg', type: 'image', name: 'BRB — Static', visible: true, locked: false }],
  webcam: [{ id: 'cam', type: 'camera', name: 'Webcam — α7C (full)', visible: true, locked: false }],
  ending: [{ id: 'card', type: 'text', name: 'Thanks for watching', visible: true, locked: false }],
};

function SourcesPanel({ active, selected, onSelect }) {
  const list = SOURCES_BY_SCENE[active] || [];
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, flex: '1 1 auto', minHeight: 0 }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="display" size={14}/> Sources</div>
        <div className="panel-hd-tools">
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt="Add source"><Icon name="plus" size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost"><Icon name="more" size={14}/></button>
        </div>
      </div>
      <div style={{ padding: '4px 6px', flex: 1, overflow: 'auto' }}>
        {list.map(s => (
          <div key={s.id} className="row" data-active={selected === s.id} onClick={() => onSelect(s.id)} style={{ cursor: 'pointer', height: 30 }}>
            <Icon name={typeIcon(s.type)} size={12} style={{ color: 'var(--text-mute)' }}/>
            <span style={{ fontSize: 'var(--fs-sm)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{s.name}</span>
            <div style={{ marginLeft: 'auto', display: 'inline-flex', alignItems: 'center', gap: 4 }}>
              {s.audio && <Icon name="speaker" size={11} style={{ color: 'var(--text-mute)' }}/>}
              <button className="btn-ghost" style={{ padding: 2, color: s.visible ? 'var(--text-dim)' : 'var(--text-faint)' }}><Icon name={s.visible ? 'eye' : 'eyeOff'} size={12}/></button>
              <button className="btn-ghost" style={{ padding: 2, color: s.locked ? 'var(--accent)' : 'var(--text-faint)' }}><Icon name={s.locked ? 'lock' : 'unlock'} size={12}/></button>
            </div>
          </div>
        ))}
        {list.length === 0 && (
          <div style={{ padding: 'var(--s-4) var(--s-3)', textAlign: 'center', color: 'var(--text-mute)', fontSize: 'var(--fs-sm)' }}>
            No sources in this scene.<br/>
            <button className="btn btn-sm" style={{ marginTop: 8 }}><Icon name="plus" size={12}/>Add a source</button>
          </div>
        )}
      </div>
      {/* Source type quick-add */}
      <div style={{ padding: 'var(--s-2)', borderTop: '1px solid var(--border)', display: 'flex', gap: 4, flexWrap: 'wrap' }}>
        {[
          { t: 'Display', i: 'display' },
          { t: 'Window',  i: 'window'  },
          { t: 'Camera',  i: 'camera'  },
          { t: 'Mic',     i: 'mic'     },
          { t: 'Image',   i: 'image'   },
          { t: 'Text',    i: 'text'    },
          { t: 'Browser', i: 'browser' },
          { t: 'Color',   i: 'layers'  },
        ].map(s => (
          <button key={s.t} className="btn btn-sm btn-ghost tt-wrap" data-tt={`Add ${s.t}`} style={{ height: 24, padding: '0 6px' }}>
            <Icon name={s.i} size={11}/><span style={{ fontSize: 11 }}>{s.t}</span>
          </button>
        ))}
      </div>
    </div>
  );
}

function typeIcon(t) {
  return ({ window: 'window', camera: 'camera', mic: 'mic', image: 'image', browser: 'browser', text: 'text', display: 'display' })[t] || 'layers';
}

// ─── Preview area ───
function PreviewArea({ sim, activeScene }) {
  const isRec = sim.mode === 'recording';
  const isLive = sim.mode === 'streaming';
  return (
    <div style={{ flex: 1, minHeight: 0, padding: 'var(--s-4)', background: 'var(--bg-base)', display: 'flex', flexDirection: 'column', gap: 'var(--s-3)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--s-2)', flex: '0 0 auto' }}>
        <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
          <button className="btn btn-sm" style={{ height: 24, background: 'var(--surface-2)', border: 0 }}>Program</button>
          <button className="btn btn-sm btn-ghost" style={{ height: 24, padding: '0 8px' }}>Preview</button>
          <button className="btn btn-sm btn-ghost" style={{ height: 24, padding: '0 8px' }}>Multiview</button>
        </div>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6, alignItems: 'center' }}>
          {isRec && <span className="tag tag-rec"><span className="dot-pulse" style={{ width: 6, height: 6 }}/>RECORDING</span>}
          {isLive && <span className="tag tag-live"><span className="dot-live" style={{ width: 6, height: 6 }}/>LIVE</span>}
          <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>Scene: <span style={{ color: 'var(--text)' }}>{SCENES.find(s=>s.id===activeScene)?.label}</span></span>
        </div>
      </div>

      <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
        <div style={{ position: 'relative', maxWidth: '100%', maxHeight: '100%', aspectRatio: '16/9', width: '100%' }}>
          <Placeholder label={`Scene · ${activeScene}`} ratio="16/9" radius={8} style={{ width: '100%', height: '100%' }}/>
          {/* Composited source rectangles (illustrative) */}
          {activeScene === 'gameplay' && (
            <>
              <div style={{ position: 'absolute', right: '2.5%', bottom: '6%', width: '22%', aspectRatio: '16/9', border: '1.5px solid var(--accent)', borderRadius: 4, background: 'oklch(0 0 0 / 0.5)', display: 'flex', alignItems: 'flex-end', padding: 4 }}>
                <span className="mono" style={{ fontSize: 9, color: 'var(--text)' }}>cam.α7C</span>
              </div>
              <div style={{ position: 'absolute', left: '4%', top: '5%', padding: '4px 8px', background: 'oklch(0 0 0 / 0.55)', borderRadius: 4 }}>
                <span className="mono" style={{ fontSize: 11, color: 'var(--text)' }}>NO-HIT ATTEMPT</span>
              </div>
              <div style={{ position: 'absolute', left: '50%', bottom: '4%', transform: 'translateX(-50%)', width: '40%', height: '6%', background: 'oklch(0 0 0 / 0.55)', borderRadius: 4, display: 'flex', alignItems: 'center', gap: 6, padding: '0 8px' }}>
                <span style={{ width: 8, height: 8, borderRadius: 999, background: 'var(--rec)' }}/>
                <span className="mono" style={{ fontSize: 9, color: 'var(--text-dim)' }}>LIVE · 4,201 viewers</span>
              </div>
            </>
          )}
          {/* Safe area overlay */}
          <div style={{ position: 'absolute', inset: '5%', border: '1px dashed oklch(0.97 0 0 / 0.18)', borderRadius: 4, pointerEvents: 'none' }}/>
        </div>
      </div>

      {/* Preview tools */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, flex: '0 0 auto' }}>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Snap guides"><Icon name="grid" size={12}/></button>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Safe areas"><Icon name="image" size={12}/></button>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Zoom out"><Icon name="zoomOut" size={12}/></button>
        <span className="mono mute" style={{ fontSize: 'var(--fs-xs)', minWidth: 40, textAlign: 'center' }}>Fit</span>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Zoom in"><Icon name="zoomIn" size={12}/></button>
        <span style={{ marginLeft: 'auto', display: 'inline-flex', gap: 14, alignItems: 'center' }}>
          <Stat label="X"     value="0,0"/>
          <Stat label="SIZE"  value="1920 × 1080"/>
          <Stat label="DROPPED" value="0" tone="ok"/>
        </span>
      </div>
    </div>
  );
}

// ─── Controls bar (bottom of preview) ───
function ControlsBar({ sim, rec, transition, setTransition, profile, setProfile }) {
  const isRec = sim.mode === 'recording';
  const isLive = sim.mode === 'streaming';
  return (
    <div style={{ flex: '0 0 auto', borderTop: '1px solid var(--border)', background: 'var(--surface-0)', padding: 'var(--s-3) var(--s-4)', display: 'flex', alignItems: 'center', gap: 'var(--s-3)' }}>
      {/* Mode selector */}
      <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
        <button className="btn btn-sm" style={{ height: 24, background: 'var(--surface-2)' }}><Icon name="record" size={11}/>Record</button>
        <button className="btn btn-sm btn-ghost" style={{ height: 24 }}><Icon name="stream" size={11}/>Stream</button>
        <button className="btn btn-sm btn-ghost" style={{ height: 24 }}><Icon name="layers" size={11}/>Both</button>
      </div>

      {/* Transition */}
      <div style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>Transition</span>
        <select className="input" style={{ width: 110 }} value={transition} onChange={(e) => setTransition(e.target.value)}>
          <option value="cut">Cut</option>
          <option value="fade">Fade · 300ms</option>
          <option value="slide">Slide</option>
        </select>
      </div>

      {/* Profile */}
      <div style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>Profile</span>
        <select className="input" style={{ width: 220 }} value={profile} onChange={(e) => setProfile(e.target.value)}>
          <option>NVENC · 1080p60 · 8 Mb/s</option>
          <option>NVENC · 1440p60 · 12 Mb/s</option>
          <option>x264 · 1080p30 · 4 Mb/s</option>
        </select>
      </div>

      {/* Timer */}
      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 'var(--s-3)' }}>
        <span className="mono" style={{ fontSize: 16, fontWeight: 600, color: isRec ? 'var(--rec-hi)' : isLive ? 'var(--rec-hi)' : 'var(--text-dim)', minWidth: 96, textAlign: 'right' }}>
          {isRec ? rec.text : '00:00:00'}
        </span>

        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Save replay (last 30s)  ⌥⌘C">
          <Icon name="scissors" size={12}/>Clip
        </button>

        {isRec ? (
          <button className="btn btn-rec btn-lg" style={{ minWidth: 140 }}>
            <Icon name="stop" size={12}/>Stop
            <span className="kbd" style={{ background: 'oklch(0 0 0 / 0.25)', color: 'oklch(0.97 0 0)', borderColor: 'transparent' }}>F9</span>
          </button>
        ) : (
          <button className="btn btn-rec btn-lg" style={{ minWidth: 140 }}>
            <Icon name="record" size={12}/>Start
            <span className="kbd" style={{ background: 'oklch(0 0 0 / 0.25)', color: 'oklch(0.97 0 0)', borderColor: 'transparent' }}>F9</span>
          </button>
        )}

        {isLive ? (
          <button className="btn btn-lg" style={{ borderColor: 'var(--rec)', color: 'var(--rec-hi)' }}><Icon name="stream" size={12}/>End Stream</button>
        ) : (
          <button className="btn btn-lg"><Icon name="stream" size={12}/>Go Live</button>
        )}
      </div>
    </div>
  );
}

// ─── Audio Mixer ───
function AudioMixer({ channels }) {
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, borderBottom: '1px solid var(--border)', flex: '0 0 auto' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="speaker" size={14}/> Audio Mixer</div>
        <div className="panel-hd-tools">
          <button className="btn btn-icon btn-sm btn-ghost tt-wrap" data-tt="Add input"><Icon name="plus" size={12}/></button>
          <button className="btn btn-icon btn-sm btn-ghost"><Icon name="more" size={14}/></button>
        </div>
      </div>
      <div style={{ padding: 'var(--s-3)', display: 'flex', flexDirection: 'column', gap: 'var(--s-3)' }}>
        {channels.map((c, i) => <MixerStrip key={i} {...c}/>)}
      </div>
    </div>
  );
}

function MixerStrip({ name, sub, level, peak, muted }) {
  const [gain, setGain] = useState(75);
  const lvl = muted ? 0 : level;
  const pk = muted ? 0 : peak;
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8 }}>
        <span style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>{name}</span>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{sub}</span>
        <span className="mono mute" style={{ marginLeft: 'auto', fontSize: 10 }}>{pk < 0.001 ? '-∞' : `${(20*Math.log10(Math.max(pk,0.001))).toFixed(1)} dB`}</span>
      </div>
      <VuMeter level={lvl} peak={pk} height={8}/>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt={muted ? 'Unmute' : 'Mute'} style={{ width: 24, padding: 0, color: muted ? 'var(--rec)' : 'var(--text-dim)' }}>
          <Icon name={muted ? 'micOff' : 'mic'} size={12}/>
        </button>
        <input type="range" min="0" max="100" value={gain} onChange={(e) => setGain(+e.target.value)} style={{ flex: 1, accentColor: 'var(--accent)' }}/>
        <span className="mono mute" style={{ fontSize: 10, width: 40, textAlign: 'right' }}>{(gain - 75).toFixed(0)} dB</span>
        <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Effects" style={{ width: 24, padding: 0 }}>
          <Icon name="bolt" size={12}/>
        </button>
      </div>
    </div>
  );
}

// ─── Source Inspector ───
function SourceInspector({ selected }) {
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, flex: 1, minHeight: 0 }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="settings" size={14}/> Inspector — Webcam · α7C</div>
      </div>
      <div style={{ padding: 'var(--s-3)', flex: 1, overflow: 'auto', display: 'flex', flexDirection: 'column', gap: 'var(--s-4)' }}>
        <Field label="Transform">
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 6 }}>
            <Input label="X"      value="1488" suffix="px"/>
            <Input label="Y"      value="780"  suffix="px"/>
            <Input label="Width"  value="420"  suffix="px"/>
            <Input label="Height" value="236"  suffix="px"/>
            <Input label="Rotate" value="0"    suffix="°"/>
            <Input label="Opacity" value="100" suffix="%"/>
          </div>
        </Field>

        <Field label="Filters">
          <div className="card-strong" style={{ background: 'var(--surface-1)', borderRadius: 'var(--r-2)', overflow: 'hidden' }}>
            {[
              { name: 'Color Correction', en: true },
              { name: 'Chroma Key',       en: false },
              { name: 'Sharpen',          en: true },
            ].map((f, i) => (
              <div key={i} style={{
                display: 'flex', alignItems: 'center', gap: 8,
                padding: '8px 10px',
                borderBottom: i < 2 ? '1px solid var(--border)' : 'none',
              }}>
                <input type="checkbox" defaultChecked={f.en} style={{ accentColor: 'var(--accent)' }}/>
                <span style={{ fontSize: 'var(--fs-sm)', color: f.en ? 'var(--text)' : 'var(--text-mute)' }}>{f.name}</span>
                <button className="btn btn-sm btn-ghost" style={{ marginLeft: 'auto', padding: 2 }}><Icon name="more" size={12}/></button>
              </div>
            ))}
            <button className="btn btn-sm btn-ghost" style={{ width: '100%', justifyContent: 'center', borderRadius: 0 }}>
              <Icon name="plus" size={11}/>Add Filter
            </button>
          </div>
        </Field>

        <Field label="Properties · Camera">
          <Input label="Device"   value="Sony α7C"/>
          <Input label="Resolution" value="1920 × 1080"/>
          <Input label="Framerate"  value="60 fps"/>
        </Field>
      </div>
    </div>
  );
}

function Field({ label, children }) {
  return (
    <div>
      <div style={{ fontSize: 'var(--fs-xs)', textTransform: 'uppercase', letterSpacing: '0.08em', color: 'var(--text-mute)', fontWeight: 500, marginBottom: 6 }}>{label}</div>
      {children}
    </div>
  );
}

function Input({ label, value, suffix }) {
  return (
    <label style={{ display: 'flex', flexDirection: 'column', gap: 3, marginBottom: 4 }}>
      <span className="mute" style={{ fontSize: 10 }}>{label}</span>
      <div style={{ position: 'relative' }}>
        <input className="input mono" defaultValue={value} style={{ width: '100%', fontSize: 12 }}/>
        {suffix && <span className="mono mute" style={{ position: 'absolute', right: 8, top: '50%', transform: 'translateY(-50%)', fontSize: 10 }}>{suffix}</span>}
      </div>
    </label>
  );
}

Object.assign(window, { RecordingStudio });
