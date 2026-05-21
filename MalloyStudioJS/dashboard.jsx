// dashboard.jsx — Project hub home screen.

function Dashboard() {
  const sim = useSim();
  const recT = useTimer(sim.mode === 'recording');
  const strT = useTimer(sim.mode === 'streaming');
  const mic = useFakeLevel(true, 0.4, 0.25);
  const desk = useFakeLevel(true, 0.55, 0.2);

  return (
    <div style={{ overflow: 'auto', padding: 'var(--s-6) var(--s-8) var(--s-8)', display: 'flex', flexDirection: 'column', gap: 'var(--s-6)' }}>
      {/* Greeting + quick actions */}
      <section style={{ display: 'grid', gridTemplateColumns: '1.4fr 1fr', gap: 'var(--s-5)' }}>
        <HeroCard sim={sim} recT={recT} strT={strT} />
        <QuickActions />
      </section>

      {/* Three-column row: Now + Recordings + Render */}
      <section style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 'var(--s-5)' }}>
        <NowPlayingCard sim={sim} mic={mic} desk={desk}/>
        <RecentRecordingsCard />
        <RenderQueueCard sim={sim}/>
      </section>

      {/* Two-column row: Projects + Clips */}
      <section style={{ display: 'grid', gridTemplateColumns: '1.3fr 1fr', gap: 'var(--s-5)' }}>
        <RecentProjectsCard />
        <RecentClipsCard />
      </section>

      {/* System status row */}
      <section>
        <SystemStatusCard />
      </section>
    </div>
  );
}

// ─── Hero ───
function HeroCard({ sim, recT, strT }) {
  const isIdle = sim.mode === 'idle';
  return (
    <div className="card" style={{ padding: 'var(--s-6)', position: 'relative', overflow: 'hidden', minHeight: 220 }}>
      <div style={{ position: 'absolute', inset: 0, background: 'radial-gradient(ellipse at 90% 0%, var(--accent-glass), transparent 50%)', pointerEvents: 'none' }}/>
      <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', gap: 'var(--s-4)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--s-2)' }}>
          <span className="tag tag-accent">Project</span>
          <span className="mute" style={{ fontSize: 'var(--fs-sm)' }}>Stream Night · Tuesday · 2 hr planned</span>
        </div>
        <h1 style={{ margin: 0, fontSize: 'var(--fs-3xl)', fontWeight: 600, letterSpacing: '-0.02em', lineHeight: 1.1 }}>
          Spire of the Hollow Sun <span className="mute" style={{ fontWeight: 400 }}>— Ep. 14</span>
        </h1>
        <div className="mute" style={{ fontSize: 'var(--fs-md)', maxWidth: 560 }}>
          Last opened 2 hours ago. 3 scenes, 8 sources, replay buffer ready. Encoder profile <span className="mono" style={{ color: 'var(--text)' }}>NVENC H.264 · 1080p60 · 8 Mb/s</span>.
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--s-3)', marginTop: 'var(--s-2)' }}>
          {sim.mode === 'recording' ? (
            <button className="btn btn-rec btn-lg"><span className="dot-pulse" style={{ background: 'oklch(0.95 0 0)'}}/><span className="mono" style={{ fontWeight: 600 }}>STOP · {recT.text}</span></button>
          ) : (
            <button className="btn btn-rec btn-lg"><Icon name="record" size={14}/>Start Recording <span className="kbd" style={{ background: 'oklch(0 0 0 / 0.2)', color: 'oklch(0.95 0 0)', borderColor: 'oklch(0 0 0 / 0.2)' }}>F9</span></button>
          )}
          {sim.mode === 'streaming' ? (
            <button className="btn btn-lg" style={{ borderColor: 'var(--rec)', color: 'var(--rec-hi)' }}><span className="dot-live"/><span className="mono">END STREAM · {strT.text}</span></button>
          ) : (
            <button className="btn btn-lg"><Icon name="stream" size={14}/>Go Live</button>
          )}
          <button className="btn btn-lg btn-ghost"><Icon name="editor" size={14}/>Open Editor</button>
        </div>

        <div style={{ display: 'flex', alignItems: 'center', gap: 'var(--s-5)', marginTop: 'var(--s-2)', flexWrap: 'wrap' }}>
          <FactPill label="Resolution" value="1920 × 1080" />
          <FactPill label="Framerate"  value="60 fps" />
          <FactPill label="Audio"      value="48 kHz · Stereo" />
          <FactPill label="Mic"        value="Shure SM7B" />
          <FactPill label="Destination" value="Twitch · /malloy_live" />
        </div>
      </div>
    </div>
  );
}

function FactPill({ label, value }) {
  return (
    <div style={{ display: 'inline-flex', flexDirection: 'column', gap: 2 }}>
      <span className="mute" style={{ fontSize: 10, textTransform: 'uppercase', letterSpacing: '0.08em', fontWeight: 500 }}>{label}</span>
      <span className="mono" style={{ fontSize: 'var(--fs-sm)', color: 'var(--text)', fontWeight: 500 }}>{value}</span>
    </div>
  );
}

// ─── Quick actions ───
function QuickActions() {
  const items = [
    { icon: 'record',   label: 'Start Recording', sub: 'Use last profile · F9',          tone: 'rec' },
    { icon: 'stream',   label: 'Go Live',         sub: 'Twitch · Stream Setup',          tone: 'accent' },
    { icon: 'editor',   label: 'Open Editor',     sub: 'Resume "Ep. 14 Highlights"',     tone: '' },
    { icon: 'upload',   label: 'Import Media',    sub: 'Drag files or browse',           tone: '' },
    { icon: 'clips',    label: 'Clip last 30 s',  sub: 'Replay buffer · ⌥⌘C',           tone: '' },
    { icon: 'projects', label: 'New Project',     sub: 'From scratch or template',       tone: '' },
  ];
  return (
    <div className="card" style={{ padding: 'var(--s-4)' }}>
      <SectionH>Quick actions</SectionH>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 'var(--s-2)', marginTop: 4 }}>
        {items.map((it, i) => (
          <button key={i} style={{
            display: 'flex', alignItems: 'center', gap: 'var(--s-3)',
            padding: 'var(--s-3)',
            background: 'var(--surface-1)',
            border: '1px solid var(--border)',
            borderRadius: 'var(--r-3)',
            textAlign: 'left',
            color: 'var(--text)',
            transition: 'all 100ms',
          }}
          onMouseEnter={(e) => { e.currentTarget.style.background = 'var(--surface-2)'; e.currentTarget.style.borderColor = 'var(--border-strong)'; }}
          onMouseLeave={(e) => { e.currentTarget.style.background = 'var(--surface-1)'; e.currentTarget.style.borderColor = 'var(--border)'; }}>
            <span style={{
              width: 32, height: 32, borderRadius: 'var(--r-2)',
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              background: it.tone === 'rec' ? 'var(--rec-glass)' : it.tone === 'accent' ? 'var(--accent-glass)' : 'var(--surface-2)',
              color: it.tone === 'rec' ? 'var(--rec-hi)' : it.tone === 'accent' ? 'var(--accent-hi)' : 'var(--text-dim)',
              border: '1px solid var(--border)',
            }}>
              <Icon name={it.icon} size={16}/>
            </span>
            <div style={{ display: 'flex', flexDirection: 'column', minWidth: 0 }}>
              <span style={{ fontWeight: 500, fontSize: 'var(--fs-sm)' }}>{it.label}</span>
              <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{it.sub}</span>
            </div>
          </button>
        ))}
      </div>
    </div>
  );
}

// ─── Now / live signal ───
function NowPlayingCard({ sim, mic, desk }) {
  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
      <div className="panel-hd">
        <div className="panel-hd-title">
          {sim.mode === 'recording' ? <><span className="dot-pulse"/> Now recording</>
            : sim.mode === 'streaming' ? <><span className="dot-live"/> Live signal</>
            : <><Icon name="display" size={14}/> Preview signal</>}
        </div>
        <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>1920 × 1080 · 60 fps</span>
      </div>
      <div style={{ padding: 'var(--s-3)' }}>
        <div style={{ position: 'relative' }}>
          <Placeholder label="Scene: Gameplay · Spire" ratio="16/9" radius={6}/>
          {(sim.mode === 'recording' || sim.mode === 'streaming') && (
            <div style={{ position: 'absolute', top: 8, left: 8, display: 'flex', gap: 6 }}>
              <span className="tag tag-rec"><span className="dot-pulse" style={{ width: 6, height: 6 }}/>{sim.mode === 'streaming' ? 'LIVE' : 'REC'}</span>
              <span className="tag mono">CAM</span>
              <span className="tag mono">MIC</span>
            </div>
          )}
        </div>

        <div style={{ marginTop: 'var(--s-3)', display: 'flex', flexDirection: 'column', gap: 6 }}>
          <MeterRow label="Mic — SM7B" {...mic}/>
          <MeterRow label="Desktop Audio" {...desk}/>
        </div>
      </div>
    </div>
  );
}

function MeterRow({ label, level, peak }) {
  const db = (lvl) => lvl < 0.001 ? '-∞' : `${(20 * Math.log10(lvl)).toFixed(1)}`;
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '110px 1fr 48px', alignItems: 'center', gap: 8 }}>
      <span style={{ fontSize: 'var(--fs-xs)', color: 'var(--text-dim)' }}>{label}</span>
      <VuMeter level={level} peak={peak}/>
      <span className="mono" style={{ fontSize: 'var(--fs-xs)', textAlign: 'right', color: 'var(--text-dim)' }}>{db(peak)} dB</span>
    </div>
  );
}

// ─── Recent recordings ───
function RecentRecordingsCard() {
  const recs = [
    { name: 'Spire — Ep 14 raw.mkv',    dur: '01:47:21', size: '14.2 GB', when: 'Today, 1h ago',  status: 'ok' },
    { name: 'Spire — Ep 13 raw.mkv',    dur: '02:11:08', size: '17.8 GB', when: 'Mon · 8:14 PM', status: 'ok' },
    { name: 'Late test stream.mkv',     dur: '00:23:55', size: '3.0 GB',  when: 'Sun · 11:02 PM', status: 'warn' },
    { name: 'Voice memo — intro.mp3',   dur: '00:01:12', size: '1.1 MB',  when: 'Sun · 9:45 PM',  status: 'ok' },
  ];
  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="record" size={14}/> Recent recordings</div>
        <button className="btn btn-sm btn-ghost">View all</button>
      </div>
      <div style={{ padding: '4px 6px' }}>
        {recs.map((r, i) => (
          <div key={i} className="row" style={{ height: 38 }}>
            <span style={{ width: 26, height: 26, borderRadius: 4, background: 'var(--surface-2)', display: 'inline-flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-mute)' }}><Icon name="record" size={12}/></span>
            <div style={{ display: 'flex', flexDirection: 'column', minWidth: 0, gap: 0 }}>
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', fontSize: 'var(--fs-sm)' }}>{r.name}</span>
              <span className="mute mono" style={{ fontSize: 10 }}>{r.dur} · {r.size}</span>
            </div>
            <span className="mute" style={{ marginLeft: 'auto', fontSize: 'var(--fs-xs)' }}>{r.when}</span>
            {r.status === 'warn' && <span className="tag tag-warn" style={{ marginLeft: 6 }}>truncated</span>}
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── Render queue card ───
function RenderQueueCard({ sim }) {
  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="render" size={14}/> Render queue</div>
        <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>2 active · 1 pending</span>
      </div>
      <div style={{ padding: 'var(--s-3)', display: 'flex', flexDirection: 'column', gap: 'var(--s-3)' }}>
        <RenderJob name="Ep 14 — Highlights.mp4"  pct={64} eta="3:42" running={sim.mode === 'rendering'} active />
        <RenderJob name="Ep 14 — Full episode.mp4" pct={12} eta="—"    running={sim.mode === 'rendering'} />
        <RenderJob name="Vlog — Intro reroll.mp4"  pct={0}  eta="queued" />
        <div className="mute" style={{ display: 'flex', justifyContent: 'space-between', fontSize: 'var(--fs-xs)', marginTop: 4 }}>
          <span>Output → <span className="mono" style={{ color: 'var(--text-dim)' }}>D:\Renders\Spire\</span></span>
          <button className="btn btn-sm btn-ghost">Open folder</button>
        </div>
      </div>
    </div>
  );
}

function RenderJob({ name, pct, eta, running, active }) {
  return (
    <div>
      <div style={{ display: 'flex', justifyContent: 'space-between', gap: 8, alignItems: 'center', marginBottom: 4 }}>
        <span style={{ fontSize: 'var(--fs-sm)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{name}</span>
        <span className="mono mute" style={{ fontSize: 'var(--fs-xs)', whiteSpace: 'nowrap' }}>{pct}% · ETA {eta}</span>
      </div>
      <div style={{ position: 'relative', height: 4, background: 'var(--surface-2)', borderRadius: 2, overflow: 'hidden' }}>
        <div style={{ position: 'absolute', inset: 0, width: `${pct}%`, background: active ? 'var(--accent)' : 'var(--surface-3)', transition: 'width 600ms ease' }}/>
        {active && running && <div style={{ position: 'absolute', top: 0, bottom: 0, width: 40, left: `${Math.max(0,pct-10)}%`, background: 'linear-gradient(90deg, transparent, oklch(0.97 0 0 / 0.18), transparent)', animation: 'spin 1.6s linear infinite' }}/>}
      </div>
    </div>
  );
}

// ─── Recent projects ───
function RecentProjectsCard() {
  const projects = [
    { name: 'Spire of the Hollow Sun', last: '2 hr ago',  size: '142 GB', cover: 'Scene: gameplay' },
    { name: 'Tuesday Vlog — Week 22',  last: 'Yesterday', size: '38 GB',  cover: 'Webcam · A-roll' },
    { name: 'Coding Sessions · S3',    last: '4 days',    size: '88 GB',  cover: 'Editor capture' },
    { name: 'Boss Rush — Compilation', last: '2 weeks',   size: '21 GB',  cover: 'Final cut · v3' },
  ];
  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="projects" size={14}/> Recent projects</div>
        <div style={{ display: 'flex', gap: 4 }}>
          <button className="btn btn-sm"><Icon name="plus" size={12}/>New</button>
          <button className="btn btn-sm btn-ghost">All projects</button>
        </div>
      </div>
      <div style={{ padding: 'var(--s-3)', display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 'var(--s-3)' }}>
        {projects.map((p, i) => (
          <div key={i} style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
            <Placeholder label={p.cover} ratio="16/10" radius={6}/>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 0 }}>
              <span style={{ fontSize: 'var(--fs-sm)', fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{p.name}</span>
              <span className="mute mono" style={{ fontSize: 10 }}>{p.last} · {p.size}</span>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── Recent clips ───
function RecentClipsCard() {
  const clips = [
    { dur: '0:24', tag: 'Spire',  label: 'No-hit boss phase 3' },
    { dur: '0:18', tag: 'Spire',  label: 'Dodge into riposte' },
    { dur: '0:09', tag: 'Vlog',   label: 'Bit about coffee' },
    { dur: '0:42', tag: 'Coding', label: 'Refactor reveal' },
  ];
  return (
    <div className="card" style={{ display: 'flex', flexDirection: 'column' }}>
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="clips" size={14}/> Recent clips</div>
        <button className="btn btn-sm btn-ghost">Clips library</button>
      </div>
      <div style={{ padding: 'var(--s-3)', display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 'var(--s-3)' }}>
        {clips.map((c, i) => (
          <div key={i} style={{ position: 'relative' }}>
            <Placeholder label={c.label} ratio="16/9" radius={6}/>
            <span className="tag" style={{ position: 'absolute', top: 6, left: 6 }}>{c.tag}</span>
            <span className="tag mono" style={{ position: 'absolute', bottom: 6, right: 6, background: 'oklch(0 0 0 / 0.7)', borderColor: 'transparent', color: 'var(--text)' }}>{c.dur}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─── System status ───
function SystemStatusCard() {
  const checks = [
    { icon: 'mic',     label: 'Microphone',        value: 'Shure SM7B (USB)',       ok: true },
    { icon: 'camera',  label: 'Camera',            value: 'Sony α7C · 1080p60',     ok: true },
    { icon: 'display', label: 'Display capture',   value: 'DXGI · Monitor 2',       ok: true },
    { icon: 'window',  label: 'Window capture',    value: 'Spire of the Hollow Sun', ok: true },
    { icon: 'speaker', label: 'Desktop audio',     value: 'WASAPI loopback · 48k',  ok: true },
    { icon: 'cpu',     label: 'Encoder',           value: 'NVENC H.264 ready',      ok: true },
    { icon: 'disk',    label: 'Storage',           value: '412 GB free · D:\\Renders', ok: true },
    { icon: 'link',    label: 'Stream key',        value: 'Twitch · verified',      ok: true },
  ];
  return (
    <div className="card">
      <div className="panel-hd">
        <div className="panel-hd-title"><Icon name="info" size={14}/> System status</div>
        <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>All systems ready · checked just now</span>
      </div>
      <div style={{ padding: 'var(--s-3)', display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 'var(--s-2)' }}>
        {checks.map((c, i) => (
          <div key={i} style={{
            display: 'flex', alignItems: 'center', gap: 'var(--s-3)',
            padding: 'var(--s-3)',
            background: 'var(--surface-1)',
            border: '1px solid var(--border)',
            borderRadius: 'var(--r-2)',
          }}>
            <span style={{
              width: 28, height: 28, borderRadius: 'var(--r-2)',
              background: 'var(--surface-2)',
              display: 'inline-flex', alignItems: 'center', justifyContent: 'center',
              color: 'var(--text-dim)',
            }}>
              <Icon name={c.icon} size={14}/>
            </span>
            <div style={{ display: 'flex', flexDirection: 'column', minWidth: 0 }}>
              <span style={{ fontSize: 'var(--fs-xs)', color: 'var(--text-mute)', textTransform: 'uppercase', letterSpacing: '0.05em', fontWeight: 500 }}>{c.label}</span>
              <span className="mono" style={{ fontSize: 'var(--fs-sm)', color: c.ok ? 'var(--text)' : 'var(--warn)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.value}</span>
            </div>
            <span style={{ marginLeft: 'auto', color: c.ok ? 'var(--success)' : 'var(--warn)' }}><Icon name={c.ok ? 'check' : 'alert'} size={14}/></span>
          </div>
        ))}
      </div>
    </div>
  );
}

Object.assign(window, { Dashboard });
