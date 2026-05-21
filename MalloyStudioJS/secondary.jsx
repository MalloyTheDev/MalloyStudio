// secondary.jsx — Stream Studio, Render Queue, Settings, AI Lab, Onboarding.

// ─── Stream Studio ───
function StreamStudio() {
  const sim = useSim();
  const isLive = sim.mode === 'streaming';
  const liveT = useTimer(isLive);
  const mic = useFakeLevel(true, 0.45, 0.22);
  const desk = useFakeLevel(true, 0.55, 0.18);

  // Stream health bitrate animation
  const [health, setHealth] = useState({ bitrate: 6400, dropped: 0, ping: 28, viewers: 0 });
  useEffect(() => {
    if (!isLive) { setHealth(h => ({...h, viewers: 0})); return; }
    let v = 1240;
    const id = setInterval(() => {
      v += Math.floor((Math.random() - 0.35) * 80);
      v = Math.max(800, Math.min(8000, v));
      setHealth(h => ({
        bitrate: 5800 + Math.floor(Math.random() * 1200),
        dropped: h.dropped + (Math.random() > 0.95 ? Math.floor(Math.random() * 3) : 0),
        ping: 22 + Math.floor(Math.random() * 18),
        viewers: v,
      }));
    }, 1500);
    return () => clearInterval(id);
  }, [isLive]);

  return (
    <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
      {/* Center column: preview + go-live panel */}
      <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ flex: 1, minHeight: 0, padding: 16, display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span className="tag tag-accent">Destination</span>
            <button className="btn btn-sm" style={{ height: 26 }}>
              <span style={{ width: 14, height: 14, borderRadius: 3, background: 'oklch(0.55 0.18 285)', display: 'inline-flex' }}/>
              <span style={{ fontWeight: 500 }}>Twitch · /malloy_live</span>
              <Icon name="chevDown" size={11}/>
            </button>
            <button className="btn btn-sm btn-ghost"><Icon name="plus" size={11}/>Add destination</button>
            {isLive && <span className="tag tag-live" style={{ marginLeft: 'auto' }}><span className="dot-live"/>LIVE · {liveT.text}</span>}
          </div>

          <div style={{ flex: 1, minHeight: 0, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <div style={{ position: 'relative', width: '100%', maxHeight: '100%', aspectRatio: '16/9' }}>
              <Placeholder label="Program · Gameplay scene" ratio="16/9" radius={8} style={{ width: '100%', height: '100%' }}/>
              {isLive && (
                <div style={{ position: 'absolute', top: 12, left: 12, display: 'flex', gap: 6 }}>
                  <span className="tag tag-live"><span className="dot-live"/>LIVE</span>
                  <span className="tag mono">{health.viewers.toLocaleString()} viewers</span>
                </div>
              )}
            </div>
          </div>

          {/* Go-live panel */}
          <div className="card" style={{ padding: 14, display: 'grid', gridTemplateColumns: '1fr auto', gap: 12 }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
              <label className="mute" style={{ fontSize: 'var(--fs-xs)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>Stream title</label>
              <input className="input" defaultValue="Spire — No-hit attempt night 4 · Phase 3 grind"/>
              <div style={{ display: 'flex', gap: 8 }}>
                <select className="input" style={{ flex: 1 }}><option>Category · Spire of the Hollow Sun</option></select>
                <select className="input" style={{ width: 180 }}><option>Language · English</option></select>
              </div>
              <div style={{ display: 'flex', gap: 4, flexWrap: 'wrap' }}>
                {['Souls-like', 'No-hit', 'Phase 3', 'Late-night', 'Builds'].map(t => <span key={t} className="tag">{t}</span>)}
                <button className="btn btn-sm btn-ghost" style={{ height: 22 }}><Icon name="plus" size={11}/>Tag</button>
              </div>
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', justifyContent: 'space-between', gap: 6 }}>
              <div style={{ display: 'flex', gap: 4, justifyContent: 'flex-end' }}>
                <span className="tag">REC + STREAM</span>
                <span className="tag tag-success">key verified</span>
              </div>
              {isLive ? (
                <button className="btn btn-lg" style={{ borderColor: 'var(--rec)', color: 'var(--rec-hi)', minWidth: 200 }}>
                  <span className="dot-live"/>End stream
                </button>
              ) : (
                <button className="btn btn-lg btn-primary" style={{ minWidth: 200, background: 'var(--rec)', borderColor: 'var(--rec)', color: 'oklch(0.97 0 0)' }}>
                  <Icon name="stream" size={14}/>Go Live · F8
                </button>
              )}
            </div>
          </div>
        </div>
      </div>

      {/* Right column: health + chat + alerts */}
      <aside style={{ width: 380, borderLeft: '1px solid var(--border)', background: 'var(--surface-0)', display: 'flex', flexDirection: 'column', flex: '0 0 auto' }}>
        <StreamHealthCard health={health} isLive={isLive}/>
        <ChatDock isLive={isLive} viewers={health.viewers}/>
        <AlertsDock isLive={isLive}/>
        <div className="panel" style={{ border: 0, borderRadius: 0, borderTop: '1px solid var(--border)', flex: '0 0 auto' }}>
          <div className="panel-hd"><div className="panel-hd-title"><Icon name="speaker" size={14}/>Mix</div></div>
          <div style={{ padding: 10, display: 'flex', flexDirection: 'column', gap: 8 }}>
            <MicroMeter label="Mic" {...mic}/>
            <MicroMeter label="Desktop" {...desk}/>
          </div>
        </div>
      </aside>
    </div>
  );
}

function MicroMeter({ label, level, peak }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '60px 1fr', alignItems: 'center', gap: 6 }}>
      <span style={{ fontSize: 'var(--fs-xs)', color: 'var(--text-dim)' }}>{label}</span>
      <VuMeter level={level} peak={peak}/>
    </div>
  );
}

function StreamHealthCard({ health, isLive }) {
  const ok = isLive && health.dropped < 10;
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, borderBottom: '1px solid var(--border)', flex: '0 0 auto' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="info" size={14}/>Stream health</div>
        <span className={ok ? 'tag tag-success' : isLive ? 'tag tag-warn' : 'tag'}>{isLive ? (ok ? 'GOOD' : 'DEGRADED') : 'OFFLINE'}</span>
      </div>
      <div style={{ padding: 10, display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8 }}>
        <Metric label="Viewers"  value={isLive ? health.viewers.toLocaleString() : '—'} tone="accent"/>
        <Metric label="Bitrate"  value={isLive ? `${(health.bitrate / 1000).toFixed(1)} Mb/s` : '—'}/>
        <Metric label="Dropped"  value={isLive ? `${health.dropped}` : '—'} tone={health.dropped > 5 ? 'warn' : ''}/>
        <Metric label="Ping"     value={isLive ? `${health.ping} ms` : '—'}/>
        <Metric label="FPS"      value={isLive ? '59.8' : '—'}/>
        <Metric label="Encoder"  value="NVENC · H.264"/>
      </div>
    </div>
  );
}

function Metric({ label, value, tone }) {
  const color = tone === 'accent' ? 'var(--accent-hi)'
              : tone === 'warn'   ? 'var(--warn)'
              : tone === 'rec'    ? 'var(--rec-hi)'
              : 'var(--text)';
  return (
    <div style={{ padding: 8, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)' }}>
      <div className="mute" style={{ fontSize: 10, textTransform: 'uppercase', letterSpacing: '0.06em', fontWeight: 500 }}>{label}</div>
      <div className="mono" style={{ fontSize: 'var(--fs-md)', color, fontWeight: 600, marginTop: 2 }}>{value}</div>
    </div>
  );
}

function ChatDock({ isLive, viewers }) {
  const messages = isLive ? [
    { u: 'rekka_dev',   m: 'GG that was insane',         c: 'accent' },
    { u: 'gorm',        m: 'how did he dodge that????',  c: '' },
    { u: 'phaseseven',  m: 'phase 3 nightmare run',       c: '' },
    { u: 'lunalux',     m: 'first time here, this slaps' },
    { u: 'gorm',        m: 'POG' },
    { u: 'wrenly',      m: 'hyped for the no-hit clip' },
    { u: 'malloy_bot',  m: '🔔 phaseseven just subscribed (3 mo)', c: 'warn' },
    { u: 'rekka_dev',   m: 'whats your sens at' },
    { u: 'kx',          m: 'audio is so clean' },
  ] : [];
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, borderBottom: '1px solid var(--border)', flex: '1 1 auto', minHeight: 0 }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="browser" size={14}/>Chat</div>
        <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>{isLive ? viewers.toLocaleString() : '—'} watching</span>
      </div>
      <div style={{ flex: 1, overflow: 'auto', padding: '6px 10px', display: 'flex', flexDirection: 'column', gap: 4 }}>
        {!isLive ? (
          <div style={{ flex: 1, display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', textAlign: 'center', padding: 24, color: 'var(--text-mute)', fontSize: 'var(--fs-sm)' }}>
            <Icon name="browser" size={32} style={{ color: 'var(--text-faint)', marginBottom: 8 }}/>
            <div style={{ fontWeight: 500, color: 'var(--text-dim)' }}>Chat appears when you go live.</div>
            <div className="mute" style={{ fontSize: 'var(--fs-xs)', marginTop: 4 }}>Twitch · /malloy_live</div>
          </div>
        ) : messages.map((m, i) => (
          <div key={i} style={{ fontSize: 'var(--fs-sm)', lineHeight: 1.5 }}>
            <span style={{ color: m.c === 'accent' ? 'var(--accent-hi)' : m.c === 'warn' ? 'var(--warn)' : 'var(--text-dim)', fontWeight: 600 }}>{m.u}</span>
            <span className="mute"> · </span>
            <span style={{ color: m.c === 'warn' ? 'var(--warn)' : 'var(--text)' }}>{m.m}</span>
          </div>
        ))}
      </div>
      <div style={{ borderTop: '1px solid var(--border)', padding: 8 }}>
        <input className="input" placeholder={isLive ? 'Say something…' : 'Chat (offline)'} disabled={!isLive}/>
      </div>
    </div>
  );
}

function AlertsDock({ isLive }) {
  const alerts = isLive ? [
    { kind: 'sub',    text: 'phaseseven · Tier 1 · 3-month resub' },
    { kind: 'follow', text: 'kx_dev followed' },
    { kind: 'bits',   text: 'wrenly cheered 500 bits' },
  ] : [];
  return (
    <div className="panel" style={{ border: 0, borderRadius: 0, borderBottom: '1px solid var(--border)', flex: '0 0 auto' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="bell" size={14}/>Alerts</div>
        <button className="btn btn-sm btn-ghost">Mute</button>
      </div>
      <div style={{ padding: 10, display: 'flex', flexDirection: 'column', gap: 4, maxHeight: 140, overflow: 'auto' }}>
        {!isLive ? (
          <span className="mute" style={{ fontSize: 'var(--fs-xs)', padding: '8px 4px' }}>No alerts. Go live to start receiving them.</span>
        ) : alerts.map((a, i) => (
          <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '6px 8px', background: 'var(--surface-1)', borderRadius: 4, fontSize: 'var(--fs-sm)' }}>
            <span style={{ width: 18, height: 18, borderRadius: 999, background: 'var(--accent-glass)', display: 'inline-flex', alignItems: 'center', justifyContent: 'center', color: 'var(--accent-hi)' }}>
              <Icon name={a.kind === 'sub' ? 'star' : a.kind === 'bits' ? 'bolt' : 'plus'} size={10}/>
            </span>
            <span style={{ color: 'var(--text-dim)' }}>{a.text}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── Render Queue ───
function RenderQueue() {
  const sim = useSim();
  const isRender = sim.mode === 'rendering';
  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', display: 'flex', gap: 8, alignItems: 'center' }}>
        <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
          {[
            { id: 'active', label: 'Active', count: 2 },
            { id: 'pending', label: 'Pending', count: 3 },
            { id: 'done', label: 'Completed', count: 47 },
            { id: 'fail', label: 'Failed', count: 1 },
          ].map((t, i) => (
            <button key={t.id} className="btn btn-sm" style={{ height: 22, background: i === 0 ? 'var(--surface-2)' : 'transparent', borderColor: 'transparent', display: 'inline-flex', gap: 4 }}>
              {t.label} <span className="mono mute" style={{ fontSize: 10 }}>{t.count}</span>
            </button>
          ))}
        </div>
        <span className="mute mono" style={{ fontSize: 'var(--fs-xs)', marginLeft: 8 }}>2 active · 3 pending · 1 failed</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
          <button className="btn btn-sm btn-ghost"><Icon name="pause" size={12}/>Pause queue</button>
          <button className="btn btn-sm"><Icon name="plus" size={12}/>New render</button>
        </div>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 16, display: 'flex', flexDirection: 'column', gap: 12 }}>
        {/* Active jobs */}
        <SectionH right="Now rendering">Active</SectionH>
        <ActiveJob name="Ep 14 — Highlights.mp4"  pct={64} eta="3:42" project="Spire of the Hollow Sun"
                   src="14:32 of 22:18 · 1080p60 · NVENC · 24 Mb/s" running={isRender}/>
        <ActiveJob name="Ep 14 — Full episode.mp4" pct={12} eta="14:08" project="Spire of the Hollow Sun"
                   src="0:23:18 of 1:54:00 · 1080p60 · NVENC · 18 Mb/s" running={isRender}/>

        <SectionH>Pending</SectionH>
        <div className="card">
          <PendingRow name="Vlog — Intro reroll.mp4" project="Tuesday Vlog" target="YouTube · 1080p60" pos={1}/>
          <PendingRow name="Coding S3 — Ep 06.mp4"   project="Coding Sessions" target="YouTube · 1440p60" pos={2}/>
          <PendingRow name="Phase 3 highlight short.mp4" project="Spire" target="Shorts · 1080×1920 · 60 fps" pos={3}/>
        </div>

        <SectionH>Failed</SectionH>
        <div className="card" style={{ padding: 12, background: 'oklch(0.66 0.22 25 / 0.06)', borderColor: 'oklch(0.66 0.22 25 / 0.45)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <Icon name="alert" size={16} style={{ color: 'var(--rec-hi)' }}/>
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>Ep 14 — Director's cut.mp4</div>
              <div className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>ffmpeg exited with code 234 · libx264: encoder failed at frame 412,108</div>
            </div>
            <button className="btn btn-sm btn-ghost">View log</button>
            <button className="btn btn-sm"><Icon name="refresh" size={11}/>Retry</button>
          </div>
        </div>

        <SectionH>Recently completed</SectionH>
        <div className="card">
          {[
            { name: 'Ep 13 — Highlights.mp4',  size: '1.4 GB', dur: '22:18 · 1080p60', when: 'Yesterday 10:42 PM' },
            { name: 'Ep 13 — Full.mp4',         size: '4.2 GB', dur: '1:51:00 · 1080p60', when: 'Yesterday 10:08 PM' },
            { name: 'Vlog — Week 21.mp4',       size: '880 MB', dur: '14:12 · 1080p30', when: 'Mon 3:18 PM' },
          ].map((c, i) => (
            <div key={i} style={{ display: 'grid', gridTemplateColumns: '24px 1fr 1fr 120px 140px 80px', alignItems: 'center', gap: 8, padding: '10px 12px', borderBottom: i < 2 ? '1px solid var(--border)' : 'none' }}>
              <Icon name="check" size={14} style={{ color: 'var(--success)' }}/>
              <span style={{ fontSize: 'var(--fs-sm)' }}>{c.name}</span>
              <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>{c.dur}</span>
              <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>{c.size}</span>
              <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{c.when}</span>
              <div style={{ display: 'inline-flex', gap: 2 }}>
                <button className="btn btn-sm btn-ghost"><Icon name="folder" size={11}/></button>
                <button className="btn btn-sm btn-ghost"><Icon name="upload" size={11}/></button>
              </div>
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}

function ActiveJob({ name, pct, eta, project, src, running }) {
  return (
    <div className="card" style={{ padding: 14 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 10 }}>
        <span className="spinner"/>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 'var(--fs-md)', fontWeight: 500 }}>{name}</div>
          <div className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>{project} · {src}</div>
        </div>
        <div style={{ textAlign: 'right' }}>
          <div className="mono" style={{ fontSize: 'var(--fs-lg)', fontWeight: 600, color: 'var(--accent-hi)' }}>{pct}%</div>
          <div className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>ETA {eta}</div>
        </div>
      </div>
      <div style={{ position: 'relative', height: 6, background: 'var(--surface-2)', borderRadius: 3, overflow: 'hidden' }}>
        <div style={{ position: 'absolute', inset: 0, width: `${pct}%`, background: 'linear-gradient(90deg, var(--accent-dim), var(--accent))', transition: 'width 600ms ease' }}/>
        {running && <div style={{ position: 'absolute', top: 0, bottom: 0, width: 60, left: `${Math.max(0,pct-15)}%`, background: 'linear-gradient(90deg, transparent, oklch(0.97 0 0 / 0.2), transparent)', animation: 'spin 1.6s linear infinite' }}/>}
      </div>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginTop: 8 }}>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>Output → <span className="mono" style={{ color: 'var(--text-dim)' }}>D:\Renders\Spire\Ep14\</span></span>
        <div style={{ display: 'inline-flex', gap: 4 }}>
          <button className="btn btn-sm btn-ghost"><Icon name="pause" size={11}/>Pause</button>
          <button className="btn btn-sm btn-ghost" style={{ color: 'var(--rec-hi)' }}><Icon name="close" size={11}/>Cancel</button>
        </div>
      </div>
    </div>
  );
}

function PendingRow({ name, project, target, pos }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '32px 1fr 1fr 200px 100px', alignItems: 'center', gap: 8, padding: '10px 12px', borderBottom: '1px solid var(--border)' }}>
      <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>#{pos}</span>
      <span style={{ fontSize: 'var(--fs-sm)' }}>{name}</span>
      <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{project}</span>
      <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>{target}</span>
      <div style={{ display: 'inline-flex', gap: 4, justifyContent: 'flex-end' }}>
        <button className="btn btn-sm btn-ghost"><Icon name="chevDown" size={11} style={{ transform: 'rotate(180deg)' }}/></button>
        <button className="btn btn-sm btn-ghost"><Icon name="trash" size={11}/></button>
      </div>
    </div>
  );
}

// ─── Settings ───
function SettingsScreen() {
  const groups = [
    { id: 'general', label: 'General',    icon: 'settings' },
    { id: 'rec',     label: 'Recording',  icon: 'record' },
    { id: 'stream',  label: 'Streaming',  icon: 'stream' },
    { id: 'video',   label: 'Video',      icon: 'editor' },
    { id: 'audio',   label: 'Audio',      icon: 'speaker' },
    { id: 'hotkeys', label: 'Hotkeys',    icon: 'command' },
    { id: 'storage', label: 'Storage',    icon: 'disk' },
    { id: 'perf',    label: 'Performance', icon: 'cpu' },
    { id: 'appear',  label: 'Appearance', icon: 'image' },
    { id: 'acct',    label: 'Accounts',   icon: 'projects' },
    { id: 'exp',     label: 'Experimental', icon: 'bolt' },
    { id: 'ai',      label: 'AI placeholders', icon: 'ai', planned: true },
  ];
  const [g, setG] = useState('rec');
  return (
    <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
      <aside style={{ width: 240, borderRight: '1px solid var(--border)', background: 'var(--surface-0)', padding: '8px 6px', overflow: 'auto', flex: '0 0 auto' }}>
        {groups.map(s => (
          <div key={s.id} className="row" data-active={g === s.id} onClick={() => setG(s.id)} style={{ cursor: 'pointer' }}>
            <Icon name={s.icon} size={12} style={{ color: 'var(--text-mute)' }}/>
            <span style={{ fontSize: 'var(--fs-sm)' }}>{s.label}</span>
            {s.planned && <span className="row-meta tag" style={{ fontSize: 9, padding: '1px 4px' }}>soon</span>}
          </div>
        ))}
      </aside>
      <div style={{ flex: 1, overflow: 'auto', padding: 24, maxWidth: 800 }}>
        {g === 'rec' && <RecordingSettings/>}
        {g !== 'rec' && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 10, color: 'var(--text-dim)' }}>
            <h2 style={{ margin: 0, fontWeight: 600, fontSize: 'var(--fs-2xl)' }}>{groups.find(x => x.id === g)?.label}</h2>
            <p className="mute" style={{ marginTop: 0 }}>Section design follows the same Setting / Field / Hint pattern as Recording.</p>
            <SettingsBlock title="Sample section" items={[
              { label: 'Some option', hint: 'Hint text under input', control: <select className="input" style={{ width: 220 }}><option>Option A</option></select> },
              { label: 'Toggle option', hint: 'Hint text', control: <input type="checkbox" defaultChecked style={{ accentColor: 'var(--accent)' }}/> },
            ]}/>
          </div>
        )}
      </div>
    </div>
  );
}

function RecordingSettings() {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 24 }}>
      <div>
        <h2 style={{ margin: 0, fontWeight: 600, fontSize: 'var(--fs-2xl)' }}>Recording</h2>
        <p className="mute" style={{ marginTop: 4 }}>Encoder, container, quality, and where files are saved.</p>
      </div>
      <SettingsBlock title="Encoder" items={[
        { label: 'Hardware encoder', hint: 'Detected: NVIDIA RTX 4080 · NVENC AV1, H.264. Falls back to x264.',
          control: <select className="input" style={{ width: 260 }}><option>NVENC H.264 (recommended)</option><option>NVENC AV1</option><option>QuickSync H.264</option><option>x264 (CPU)</option></select> },
        { label: 'Rate control',    hint: 'CBR for streams, CRF for archival recordings.',
          control: <select className="input" style={{ width: 260 }}><option>CRF (quality-based)</option><option>CBR (constant bitrate)</option></select> },
        { label: 'CRF',             hint: 'Lower = higher quality. 18–23 is typical.',
          control: <input className="input" defaultValue="20" style={{ width: 100 }}/> },
        { label: 'Container',       hint: 'MKV survives crashes; MP4 is more compatible.',
          control: <select className="input" style={{ width: 260 }}><option>MKV (Matroska) · recommended</option><option>MP4</option><option>MOV</option></select> },
      ]}/>
      <SettingsBlock title="Resolution & Framerate" items={[
        { label: 'Canvas resolution', hint: 'Your scene composes to this size.',
          control: <select className="input" style={{ width: 260 }}><option>1920 × 1080</option><option>2560 × 1440</option><option>3840 × 2160</option></select> },
        { label: 'Recording FPS',     hint: 'Will be matched by all capture sources where possible.',
          control: <select className="input" style={{ width: 260 }}><option>60</option><option>30</option><option>24</option></select> },
      ]}/>
      <SettingsBlock title="Storage" items={[
        { label: 'Recording folder',  hint: '412 GB free. Recordings can be huge — use a fast NVMe drive.',
          control: <div style={{ display: 'flex', gap: 6 }}><input className="input mono" defaultValue="D:\Renders\Sessions" style={{ width: 360 }}/><button className="btn btn-sm">Browse…</button></div> },
        { label: 'Filename pattern',  hint: 'Tokens: {project} {scene} {date} {time} {n}.',
          control: <input className="input mono" defaultValue="{project} — {date} {time}" style={{ width: 360 }}/> },
        { label: 'Replay buffer',     hint: 'In-RAM ring of the last 30 seconds. Save with ⌥⌘C.',
          control: <input type="checkbox" defaultChecked style={{ accentColor: 'var(--accent)' }}/> },
      ]}/>
      <SettingsBlock title="Auto-actions" items={[
        { label: 'Auto-start when game launches', hint: 'Detect from a process list.',  control: <input type="checkbox" style={{ accentColor: 'var(--accent)' }}/> },
        { label: 'Auto-stop on inactivity',        hint: 'Helpful for unattended capture.', control: <input type="checkbox" defaultChecked style={{ accentColor: 'var(--accent)' }}/> },
        { label: 'Save clip on hotkey',            hint: 'Auto-name with timestamp.',       control: <input type="checkbox" defaultChecked style={{ accentColor: 'var(--accent)' }}/> },
      ]}/>
    </div>
  );
}

function SettingsBlock({ title, items }) {
  return (
    <div>
      <h3 style={{ margin: '0 0 12px', fontSize: 'var(--fs-md)', fontWeight: 600, textTransform: 'uppercase', letterSpacing: '0.06em', color: 'var(--text-mute)' }}>{title}</h3>
      <div className="card" style={{ overflow: 'hidden' }}>
        {items.map((it, i) => (
          <div key={i} style={{ display: 'grid', gridTemplateColumns: '1fr auto', alignItems: 'center', gap: 16, padding: '12px 16px', borderBottom: i < items.length - 1 ? '1px solid var(--border)' : 'none' }}>
            <div style={{ minWidth: 0 }}>
              <div style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>{it.label}</div>
              <div className="mute" style={{ fontSize: 'var(--fs-xs)', marginTop: 2 }}>{it.hint}</div>
            </div>
            <div>{it.control}</div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── AI Lab placeholder ───
function AILab() {
  const features = [
    { icon: 'scissors', name: 'Smart clip detection',     desc: 'Auto-detect highlights from gameplay audio + game events.' },
    { icon: 'mic',      name: 'Silence removal',          desc: 'One-pass dialogue cleanup across the timeline.' },
    { icon: 'text',     name: 'Auto subtitles',            desc: 'Whisper-driven transcription + speaker diarization.' },
    { icon: 'sparkle',  name: 'Auto chapters',             desc: 'Timeline markers + YouTube chapter list from your content.' },
    { icon: 'image',    name: 'Thumbnail variations',      desc: 'Generate 4–6 thumbnail directions from the best frame.' },
    { icon: 'bolt',     name: 'Title & description ideas', desc: 'Pull a hooky title from your transcript + tags.' },
    { icon: 'editor',   name: 'Editing assistant',         desc: 'Conversational edit: "tighten the intro, drop the dead air".' },
    { icon: 'stream',   name: 'Stream copilot',            desc: 'Suggests scene changes during slow moments.' },
    { icon: 'projects', name: 'Project organizer',         desc: 'Group orphaned clips into draft projects.' },
    { icon: 'clips',    name: 'Repurposing engine',        desc: 'Cut shorts/reels/TikToks from your long-form episodes.' },
  ];
  return (
    <div style={{ flex: 1, overflow: 'auto', padding: 32 }}>
      <div style={{ maxWidth: 960, margin: '0 auto', display: 'flex', flexDirection: 'column', gap: 24 }}>
        <div style={{ display: 'flex', alignItems: 'flex-start', gap: 16 }}>
          <div style={{ width: 56, height: 56, borderRadius: 14, background: 'var(--accent-glass)', display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--accent-hi)', flex: '0 0 auto' }}>
            <Icon name="ai" size={28}/>
          </div>
          <div>
            <div style={{ display: 'inline-flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
              <span className="tag tag-accent">Preview</span>
              <span className="mute" style={{ fontSize: 'var(--fs-sm)' }}>Planned for v0.3 · q3 2026</span>
            </div>
            <h1 style={{ margin: 0, fontWeight: 600, fontSize: 'var(--fs-3xl)', letterSpacing: '-0.02em', lineHeight: 1.1 }}>AI Lab</h1>
            <p className="dim" style={{ maxWidth: 540, marginTop: 6, fontSize: 'var(--fs-md)' }}>
              A workshop for clip detection, silence removal, subtitles, chapters, thumbnails, and conversational editing.
              Nothing here is shipping yet — the UI is here so I can wire the surfaces in early.
            </p>
            <div style={{ display: 'flex', gap: 8, marginTop: 12 }}>
              <button className="btn btn-primary"><Icon name="bell" size={12}/>Notify me when AI ships</button>
              <button className="btn btn-ghost">Read the roadmap</button>
            </div>
          </div>
        </div>

        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(2, 1fr)', gap: 12 }}>
          {features.map((f, i) => (
            <div key={i} className="card" style={{ padding: 14, display: 'flex', gap: 12, position: 'relative' }}>
              <span style={{ position: 'absolute', top: 10, right: 10, fontSize: 9, padding: '2px 5px', borderRadius: 3, background: 'var(--surface-2)', color: 'var(--text-mute)', textTransform: 'uppercase', letterSpacing: '0.06em' }}>Soon</span>
              <span style={{ width: 32, height: 32, borderRadius: 'var(--r-2)', background: 'var(--surface-2)', color: 'var(--text-dim)', display: 'flex', alignItems: 'center', justifyContent: 'center', flex: '0 0 auto' }}>
                <Icon name={f.icon} size={14}/>
              </span>
              <div>
                <div style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>{f.name}</div>
                <div className="mute" style={{ fontSize: 'var(--fs-xs)', marginTop: 3 }}>{f.desc}</div>
              </div>
            </div>
          ))}
        </div>

        <div className="card" style={{ padding: 14 }}>
          <SectionH right="Closed beta">Local-first stack</SectionH>
          <p className="dim" style={{ fontSize: 'var(--fs-sm)', margin: '6px 0 0' }}>
            All AI tasks run locally via Whisper (subtitles), CLIP/BLIP (thumbnails), and a small fine-tuned LLM for titles & chapters.
            Cloud is optional and opt-in.
          </p>
        </div>
      </div>
    </div>
  );
}

// ─── Onboarding (modal overlay) ───
function Onboarding({ open, onClose }) {
  const [step, setStep] = useState(0);
  if (!open) return null;
  const steps = [
    { title: 'Welcome to MalloyStudio',           sub: 'Record, stream, clip, and edit — in one workstation.' },
    { title: 'How will you mostly use the app?', sub: 'You can change this any time.' },
    { title: 'Where should your work live?',     sub: 'Pick a fast NVMe if you can — recordings get large.' },
    { title: 'Let’s find your devices',          sub: 'We’ll detect what you’ve got plugged in.' },
    { title: 'Pick a recording quality',          sub: 'You can tweak this per project later.' },
    { title: 'Choose a density',                  sub: 'How compact should the UI feel?' },
    { title: 'You’re ready',                      sub: 'Drop into the dashboard — or jump straight into recording.' },
  ];
  return (
    <div role="dialog" aria-modal="true" style={{ position: 'fixed', inset: 0, background: 'oklch(0 0 0 / 0.6)', backdropFilter: 'blur(4px)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 9000, padding: 24 }}>
      <div style={{ width: 720, maxHeight: '90vh', background: 'var(--surface-0)', border: '1px solid var(--border-strong)', borderRadius: 'var(--r-5)', boxShadow: 'var(--shadow-3)', overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '14px 18px', borderBottom: '1px solid var(--border)' }}>
          <BrandMark size={22}/>
          <span style={{ fontWeight: 600, fontSize: 'var(--fs-md)' }}>Setup</span>
          <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>{step + 1} / {steps.length}</span>
          <button onClick={onClose} className="btn btn-sm btn-ghost" style={{ marginLeft: 'auto' }}><Icon name="close" size={12}/></button>
        </div>

        <div style={{ position: 'relative', height: 3, background: 'var(--surface-2)' }}>
          <div style={{ position: 'absolute', inset: 0, width: `${((step + 1) / steps.length) * 100}%`, background: 'var(--accent)', transition: 'width 240ms ease' }}/>
        </div>

        <div style={{ flex: 1, padding: 28, overflow: 'auto', display: 'flex', flexDirection: 'column', gap: 18 }}>
          <div>
            <h2 style={{ margin: 0, fontSize: 'var(--fs-2xl)', fontWeight: 600, letterSpacing: '-0.01em' }}>{steps[step].title}</h2>
            <p className="mute" style={{ marginTop: 6, fontSize: 'var(--fs-md)' }}>{steps[step].sub}</p>
          </div>
          <OnboardingStep step={step}/>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '14px 18px', borderTop: '1px solid var(--border)', background: 'var(--surface-0)' }}>
          <button className="btn btn-ghost" onClick={() => setStep(Math.max(0, step - 1))} disabled={step === 0}><Icon name="chevLeft" size={12}/>Back</button>
          <button className="btn btn-ghost" onClick={onClose}>Skip setup</button>
          <div style={{ marginLeft: 'auto', display: 'inline-flex', gap: 8 }}>
            {step < steps.length - 1
              ? <button className="btn btn-primary" onClick={() => setStep(step + 1)}>Continue <Icon name="chevRight" size={12}/></button>
              : <button className="btn btn-primary" onClick={onClose}><Icon name="check" size={12}/>Open dashboard</button>}
          </div>
        </div>
      </div>
    </div>
  );
}

function OnboardingStep({ step }) {
  if (step === 0) return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
      <div className="card" style={{ padding: 16, display: 'flex', gap: 14, alignItems: 'center' }}>
        <Placeholder label="Preview · animated demo" ratio="16/9" radius={6} style={{ width: 280 }}/>
        <div>
          <div style={{ fontSize: 'var(--fs-md)', fontWeight: 500 }}>What's inside</div>
          <ul style={{ margin: '6px 0 0', paddingLeft: 20, color: 'var(--text-dim)', fontSize: 'var(--fs-sm)', lineHeight: 1.6 }}>
            <li>Capture display, window, camera, and audio</li>
            <li>Compose scenes; record &amp; stream simultaneously</li>
            <li>Trim and assemble in the built-in editor</li>
            <li>Auto-clip with the replay buffer</li>
          </ul>
        </div>
      </div>
    </div>
  );
  if (step === 1) return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(2, 1fr)', gap: 12 }}>
      {[
        { id: 'rec',  label: 'Record',         sub: 'Sessions, replays, raw footage', icon: 'record' },
        { id: 'str',  label: 'Stream',         sub: 'Twitch · YouTube · TikTok',       icon: 'stream' },
        { id: 'edit', label: 'Edit',           sub: 'Timeline, clips, render',         icon: 'editor' },
        { id: 'all',  label: 'All-in-one',     sub: 'I do everything — full layout',   icon: 'layers' },
      ].map((o, i) => (
        <button key={o.id} className="card" style={{ padding: 14, textAlign: 'left', display: 'flex', gap: 12, alignItems: 'center', cursor: 'pointer', border: i === 3 ? '1px solid var(--accent)' : '1px solid var(--border)' }}>
          <span style={{ width: 32, height: 32, borderRadius: 8, background: 'var(--surface-2)', color: 'var(--text-dim)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}><Icon name={o.icon} size={16}/></span>
          <div>
            <div style={{ fontSize: 'var(--fs-md)', fontWeight: 500 }}>{o.label}</div>
            <div className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{o.sub}</div>
          </div>
        </button>
      ))}
    </div>
  );
  if (step === 2) return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
      {[
        { name: 'D:\\Renders',         free: '412 GB free of 2 TB', tag: 'fastest' },
        { name: 'E:\\Footage Archive', free: '8 TB free of 16 TB',  tag: 'archive' },
        { name: 'C:\\Users\\jess\\Videos', free: '64 GB free of 1 TB', tag: 'system' },
      ].map((f, i) => (
        <div key={i} className="card" style={{ padding: 12, display: 'flex', alignItems: 'center', gap: 12 }}>
          <input type="radio" name="folder" defaultChecked={i === 0} style={{ accentColor: 'var(--accent)' }}/>
          <Icon name="folder" size={16} style={{ color: 'var(--text-mute)' }}/>
          <div style={{ flex: 1 }}>
            <div className="mono" style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>{f.name}</div>
            <div className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{f.free}</div>
          </div>
          <span className="tag">{f.tag}</span>
        </div>
      ))}
      <button className="btn btn-sm btn-ghost" style={{ alignSelf: 'flex-start' }}><Icon name="plus" size={11}/>Add another location</button>
    </div>
  );
  if (step === 3) return (
    <div className="card" style={{ overflow: 'hidden' }}>
      {[
        { kind: 'Microphone',        name: 'Shure SM7B (USB)',         ok: true },
        { kind: 'Webcam',            name: 'Sony α7C · 1080p60',       ok: true },
        { kind: 'Display capture',   name: 'DXGI ready · 2 monitors',  ok: true },
        { kind: 'Desktop audio',     name: 'WASAPI loopback · 48 kHz', ok: true },
        { kind: 'Window capture',    name: 'Spire of the Hollow Sun · detected', ok: true },
      ].map((d, i) => (
        <div key={i} style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '12px 16px', borderBottom: i < 4 ? '1px solid var(--border)' : 'none' }}>
          <Icon name="check" size={14} style={{ color: 'var(--success)' }}/>
          <div style={{ flex: 1, minWidth: 0 }}>
            <div style={{ fontSize: 'var(--fs-sm)', fontWeight: 500 }}>{d.kind}</div>
            <div className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>{d.name}</div>
          </div>
          <button className="btn btn-sm btn-ghost">Test</button>
        </div>
      ))}
    </div>
  );
  if (step === 4) return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 10 }}>
      {[
        { name: 'Standard',  sub: '1080p60 · 8 Mb/s · NVENC' },
        { name: 'Archival',  sub: '1080p60 · CRF 18 · NVENC · big files' },
        { name: 'Lightweight', sub: '1080p30 · 6 Mb/s · NVENC · low CPU' },
      ].map((q, i) => (
        <button key={q.name} className="card" style={{ padding: 14, textAlign: 'left', border: i === 0 ? '1px solid var(--accent)' : '1px solid var(--border)' }}>
          <div style={{ fontSize: 'var(--fs-md)', fontWeight: 500 }}>{q.name}</div>
          <div className="mute mono" style={{ fontSize: 'var(--fs-xs)', marginTop: 4 }}>{q.sub}</div>
        </button>
      ))}
    </div>
  );
  if (step === 5) return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 10 }}>
      {[
        { id: 'comfortable', label: 'Comfortable', sub: '+ negative space, bigger hit targets' },
        { id: 'compact',     label: 'Compact',      sub: 'Default. Linear-style density.' },
        { id: 'dense',       label: 'Dense',        sub: 'For pros · maximum surface area' },
      ].map(d => (
        <button key={d.id} className="card" style={{ padding: 14, textAlign: 'left', border: d.id === 'compact' ? '1px solid var(--accent)' : '1px solid var(--border)' }}>
          <div style={{ fontSize: 'var(--fs-md)', fontWeight: 500 }}>{d.label}</div>
          <div className="mute" style={{ fontSize: 'var(--fs-xs)', marginTop: 4 }}>{d.sub}</div>
        </button>
      ))}
    </div>
  );
  if (step === 6) return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 16, padding: 16 }}>
      <div style={{ width: 56, height: 56, borderRadius: 14, background: 'var(--accent-glass)', color: 'var(--accent-hi)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
        <Icon name="check" size={28}/>
      </div>
      <div style={{ textAlign: 'center', maxWidth: 360 }}>
        <div style={{ fontSize: 'var(--fs-lg)', fontWeight: 500 }}>All set.</div>
        <div className="mute" style={{ fontSize: 'var(--fs-sm)', marginTop: 4 }}>Tip: press <span className="kbd">⌘K</span> any time to find a feature or run a command.</div>
      </div>
    </div>
  );
  return null;
}

Object.assign(window, { StreamStudio, RenderQueue, SettingsScreen, AILab, Onboarding });
