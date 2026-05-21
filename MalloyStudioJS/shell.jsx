// shell.jsx — App chrome: icon rail (left), workspace header (top of workspace), status bar (bottom).
// Workspace switching is driven via SimStateContext (Tweaks can also drive it).

const NAV = [
  { id: 'dashboard', label: 'Dashboard',     icon: 'dashboard', kbd: '1' },
  { id: 'record',    label: 'Recording',     icon: 'record',    kbd: '2' },
  { id: 'stream',    label: 'Streaming',     icon: 'stream',    kbd: '3' },
  { id: 'editor',    label: 'Editor',        icon: 'editor',    kbd: '4' },
  { id: 'clips',     label: 'Clips',         icon: 'clips',     kbd: '5' },
  { id: 'media',     label: 'Media',         icon: 'media',     kbd: '6' },
  { id: 'projects',  label: 'Projects',      icon: 'projects',  kbd: '7' },
  { id: 'render',    label: 'Render Queue',  icon: 'render',    kbd: '8' },
  { id: 'ai',        label: 'AI Lab',        icon: 'ai',        kbd: '9', planned: true },
  { id: 'settings',  label: 'Settings',      icon: 'settings',  kbd: '0' },
];

function IconRail({ active, onPick, collapsed, showAI }) {
  if (collapsed) return null;
  const items = NAV.filter(n => showAI || n.id !== 'ai');
  return (
    <nav style={{
      display: 'flex', flexDirection: 'column',
      width: 'var(--rail-w)',
      borderRight: '1px solid var(--border)',
      background: 'var(--surface-0)',
      paddingTop: 8,
    }} aria-label="Workspaces">
      {/* Brand mark */}
      <div style={{
        height: 44, display: 'flex', alignItems: 'center', justifyContent: 'center',
        marginBottom: 6,
      }}>
        <BrandMark size={22} />
      </div>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 2, padding: '0 6px' }}>
        {items.map(n => {
          const isActive = active === n.id;
          return (
            <button
              key={n.id}
              className="tt-wrap"
              data-tt={`${n.label}${n.planned ? ' · planned' : ''}  ⌘${n.kbd}`}
              onClick={() => onPick(n.id)}
              aria-label={n.label}
              aria-current={isActive ? 'page' : undefined}
              style={{
                position: 'relative',
                height: 40, borderRadius: 'var(--r-2)',
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                color: isActive ? 'var(--text)' : 'var(--text-dim)',
                background: isActive ? 'var(--surface-2)' : 'transparent',
                opacity: n.planned ? 0.7 : 1,
              }}
              onMouseEnter={(e) => { if (!isActive) e.currentTarget.style.background = 'var(--surface-1)'; }}
              onMouseLeave={(e) => { if (!isActive) e.currentTarget.style.background = 'transparent'; }}
            >
              {isActive && <span style={{ position: 'absolute', left: -6, top: 6, bottom: 6, width: 2, borderRadius: 2, background: 'var(--accent)' }}/>}
              <Icon name={n.icon} size={18} />
              {n.planned && <span style={{ position: 'absolute', top: 4, right: 6, width: 5, height: 5, borderRadius: 999, background: 'var(--accent)' }}/>}
            </button>
          );
        })}
      </div>

      <div style={{ marginTop: 'auto', padding: '8px 6px', display: 'flex', flexDirection: 'column', gap: 2 }}>
        <button className="tt-wrap" data-tt="Help & shortcuts  ⌘?" style={{ height: 36, borderRadius: 'var(--r-2)', display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-mute)' }}>
          <Icon name="question" size={16}/>
        </button>
        <div style={{
          height: 32, display: 'flex', alignItems: 'center', justifyContent: 'center',
          background: 'var(--surface-1)', borderRadius: 'var(--r-2)',
          color: 'var(--text-dim)', fontFamily: 'var(--font-mono)', fontSize: 11, fontWeight: 600,
        }}>JM</div>
      </div>
    </nav>
  );
}

function BrandMark({ size = 22 }) {
  // Original geometric mark — concentric squares, accent-tinted notch.
  // Stylized "M" overlap, not borrowed from any existing brand.
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-label="MalloyStudio">
      <rect x="3" y="3" width="26" height="26" rx="6" fill="var(--surface-2)" stroke="var(--border-strong)"/>
      <path d="M9 22V10l4 6 3-5 3 5 4-6v12" stroke="var(--text)" strokeWidth="2" fill="none" strokeLinecap="round" strokeLinejoin="round"/>
      <circle cx="23" cy="9" r="2" fill="var(--accent)"/>
    </svg>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// WorkspaceHeader — title + breadcrumb + workspace-specific tabs + tools
// ─────────────────────────────────────────────────────────────────────────────
function WorkspaceHeader({ title, subtitle, tabs, activeTab, onTab, tools, projectName = 'Stream Night — Spire of the Hollow Sun' }) {
  return (
    <header style={{
      display: 'flex', alignItems: 'center',
      height: 'var(--header-h)',
      padding: '0 var(--s-4)',
      gap: 'var(--s-4)',
      borderBottom: '1px solid var(--border)',
      background: 'var(--surface-0)',
      flex: '0 0 auto',
    }}>
      <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, minWidth: 0 }}>
        <div style={{ fontWeight: 600, fontSize: 'var(--fs-lg)', letterSpacing: '-0.01em' }}>{title}</div>
        {subtitle && <div className="mute" style={{ fontSize: 'var(--fs-sm)' }}>{subtitle}</div>}
      </div>

      {tabs && tabs.length > 0 && (
        <div style={{ display: 'flex', gap: 2, marginLeft: 'var(--s-4)' }} role="tablist">
          {tabs.map(t => {
            const is = activeTab === t.id;
            return (
              <button key={t.id} role="tab" aria-selected={is} onClick={() => onTab?.(t.id)} style={{
                height: 28, padding: '0 10px',
                fontSize: 'var(--fs-sm)', fontWeight: 500,
                color: is ? 'var(--text)' : 'var(--text-dim)',
                background: is ? 'var(--surface-2)' : 'transparent',
                borderRadius: 'var(--r-2)',
                display: 'inline-flex', alignItems: 'center', gap: 6,
              }}>
                {t.icon && <Icon name={t.icon} size={14}/>}
                {t.label}
                {t.count != null && <span className="mono" style={{ fontSize: 10, color: 'var(--text-mute)' }}>{t.count}</span>}
              </button>
            );
          })}
        </div>
      )}

      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 8 }}>
        {/* Command palette pill */}
        <button className="tt-wrap" data-tt="Command palette  ⌘K" style={{
          height: 28, padding: '0 10px 0 8px',
          background: 'var(--surface-1)',
          border: '1px solid var(--border)',
          borderRadius: 'var(--r-2)',
          color: 'var(--text-mute)',
          display: 'inline-flex', alignItems: 'center', gap: 8,
          fontSize: 'var(--fs-sm)', minWidth: 220,
        }}>
          <Icon name="search" size={14}/>
          <span>Search or run command…</span>
          <span className="kbd" style={{ marginLeft: 'auto' }}>⌘K</span>
        </button>

        {/* Project pill */}
        <button className="tt-wrap" data-tt="Open project" style={{
          height: 28, padding: '0 10px',
          background: 'var(--surface-1)',
          border: '1px solid var(--border)',
          borderRadius: 'var(--r-2)',
          color: 'var(--text)',
          display: 'inline-flex', alignItems: 'center', gap: 8,
          fontSize: 'var(--fs-sm)', maxWidth: 280,
        }}>
          <Icon name="folder" size={14} style={{ color: 'var(--text-dim)'}}/>
          <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{projectName}</span>
          <Icon name="chevDown" size={12} style={{ color: 'var(--text-mute)' }}/>
        </button>

        {tools}
      </div>
    </header>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// StatusBar — bottom system bar. Reflects sim state.
// ─────────────────────────────────────────────────────────────────────────────
function StatusBar() {
  const sim = useSim();
  const mode = sim.mode;
  const isRec    = mode === 'recording';
  const isStream = mode === 'streaming';
  const isRender = mode === 'rendering';

  const recT = useTimer(isRec);
  const strT = useTimer(isStream);

  // Animated CPU/RAM/disk usage
  const [stats, setStats] = useState({ cpu: 24, ram: 38, disk: 64, fps: 60.0, bitrate: 6400 });
  useEffect(() => {
    const id = setInterval(() => {
      setStats(s => ({
        cpu:  Math.max(5, Math.min(95, s.cpu + (Math.random()-0.5) * (isRec || isStream || isRender ? 14 : 4))),
        ram:  Math.max(20, Math.min(85, s.ram + (Math.random()-0.5) * 2)),
        disk: Math.max(40, Math.min(95, s.disk + (Math.random()-0.5) * 0.4)),
        fps:  Math.max(55, Math.min(60.2, 60 - Math.random() * (isStream ? 1.5 : 0.3))),
        bitrate: 5800 + Math.floor(Math.random() * 1200),
      }));
    }, 1000);
    return () => clearInterval(id);
  }, [isRec, isStream, isRender]);

  return (
    <footer style={{
      display: 'flex', alignItems: 'center', gap: 'var(--s-4)',
      height: 'var(--status-h)',
      padding: '0 var(--s-4)',
      borderTop: '1px solid var(--border)',
      background: 'var(--surface-0)',
      fontSize: 'var(--fs-xs)',
      color: 'var(--text-dim)',
    }}>
      {/* App state indicator */}
      <div style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
        {isRec    && <><span className="dot-pulse"/><span className="mono" style={{ color: 'var(--rec-hi)', fontWeight: 600 }}>REC {recT.text}</span></>}
        {isStream && <><span className="dot-live"/><span className="mono" style={{ color: 'var(--rec-hi)', fontWeight: 600 }}>LIVE {strT.text}</span></>}
        {isRender && <><span className="spinner"/><span className="mono" style={{ color: 'var(--accent-hi)', fontWeight: 600 }}>RENDERING</span></>}
        {!isRec && !isStream && !isRender && <><span className="dot-ok"/><span className="mono">IDLE</span></>}
      </div>

      <div style={{ width: 1, height: 14, background: 'var(--border)' }}/>

      <Stat label="CPU"  value={`${stats.cpu.toFixed(0)}%`} tone={stats.cpu > 80 ? 'warn' : ''} />
      <Stat label="RAM"  value={`${stats.ram.toFixed(0)}%`}/>
      <Stat label="DISK" value={`${stats.disk.toFixed(0)}% · 412 GB free`} tone={stats.disk > 90 ? 'warn' : ''}/>
      <Stat label="GPU"  value="NVENC · 18%" />
      <Stat label="FPS"  value={stats.fps.toFixed(1)} tone={stats.fps < 58 ? 'warn' : ''}/>
      {(isRec || isStream) && <Stat label="BITRATE" value={`${(stats.bitrate/1000).toFixed(1)} Mb/s`}/>}
      {isStream && <Stat label="DROPPED" value="0.0% (0/14210)" tone="ok"/>}

      <div style={{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: 'var(--s-3)' }}>
        <span className="mono mute">v8.0.0-rc.1</span>
        <span className="mono mute">·</span>
        <span className="mono mute">ffmpeg 7.0.2</span>
        <span className="mono mute">·</span>
        <button className="tt-wrap" data-tt="Notifications" style={{ color: 'var(--text-dim)', display: 'inline-flex', alignItems: 'center', gap: 4 }}>
          <Icon name="bell" size={12}/> 3
        </button>
      </div>
    </footer>
  );
}

Object.assign(window, { IconRail, WorkspaceHeader, StatusBar, NAV, BrandMark });
