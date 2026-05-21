// app.jsx — root: router + tweaks + global state.

const TWEAK_DEFAULTS = /*EDITMODE-BEGIN*/{
  "accent": "blue",
  "bg": "charcoal",
  "density": "compact",
  "railCollapsed": false,
  "showAI": true,
  "simMode": "recording",
  "workspace": "dashboard"
}/*EDITMODE-END*/;

function MsApp() {
  const [t, setTweak] = useTweaks(TWEAK_DEFAULTS);
  const [onboardingOpen, setOnboardingOpen] = useState(false);

  // Apply data-* attrs to <html> for tokens.css variants
  useEffect(() => {
    const r = document.documentElement;
    r.setAttribute('data-accent',  t.accent);
    r.setAttribute('data-bg',      t.bg);
    r.setAttribute('data-density', t.density);
  }, [t.accent, t.bg, t.density]);

  // Keyboard shortcuts for switching workspace (1-0)
  useEffect(() => {
    const onKey = (e) => {
      if (e.target instanceof HTMLInputElement || e.target instanceof HTMLTextAreaElement) return;
      if (e.metaKey || e.ctrlKey) return;
      const idx = '1234567890'.indexOf(e.key);
      if (idx >= 0 && idx < NAV.length) {
        const item = NAV[idx];
        if (item.id === 'ai' && !t.showAI) return;
        setTweak('workspace', item.id);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [t.showAI, setTweak]);

  const sim = useMemo(() => ({
    mode: t.simMode,
    workspace: t.workspace,
    setWorkspace: (w) => setTweak('workspace', w),
    setMode: (m) => setTweak('simMode', m),
  }), [t.simMode, t.workspace, setTweak]);

  const workspace = sim.workspace;
  const meta = NAV.find(n => n.id === workspace) || NAV[0];

  // Workspace-specific header tabs/subtitles
  const tabsFor = (id) => {
    if (id === 'editor')   return [{ id: 'edit', label: 'Edit', icon: 'editor' }, { id: 'color', label: 'Color', icon: 'image' }, { id: 'audio', label: 'Audio', icon: 'speaker' }, { id: 'export', label: 'Export', icon: 'upload' }];
    if (id === 'record')   return [{ id: 'studio', label: 'Studio', icon: 'record' }, { id: 'profiles', label: 'Profiles', icon: 'settings' }, { id: 'hotkeys', label: 'Hotkeys', icon: 'command' }];
    if (id === 'stream')   return [{ id: 'live', label: 'Live', icon: 'stream' }, { id: 'dest', label: 'Destinations', icon: 'link' }, { id: 'overlays', label: 'Overlays', icon: 'layers' }];
    if (id === 'clips')    return [{ id: 'all', label: 'All', icon: 'clips', count: 42 }, { id: 'fav', label: 'Favorites', icon: 'star', count: 7 }, { id: 'arc', label: 'Archived', icon: 'folder', count: 18 }];
    if (id === 'media')    return [{ id: 'lib', label: 'Library', icon: 'media' }, { id: 'imports', label: 'Imports', icon: 'upload' }];
    if (id === 'render')   return [{ id: 'active', label: 'Active', icon: 'render', count: 2 }, { id: 'pending', label: 'Pending', icon: 'refresh', count: 3 }, { id: 'done', label: 'Completed', icon: 'check', count: 47 }];
    return [];
  };
  const subtitleFor = (id) => {
    if (id === 'dashboard') return 'Project hub';
    if (id === 'record')    return 'Live composing · 5 scenes · 7 sources';
    if (id === 'stream')    return 'Twitch · /malloy_live';
    if (id === 'editor')    return 'Ep 14 — Highlights · timeline';
    if (id === 'clips')     return '42 clips · 3.2 GB · last 60 days';
    if (id === 'media')     return '284 files · 412 GB · 1 missing';
    if (id === 'projects')  return '6 projects · 305 GB';
    if (id === 'render')    return '2 active · 3 pending · 1 failed';
    if (id === 'settings')  return 'App preferences';
    if (id === 'ai')        return 'Preview · planned features';
    return undefined;
  };

  return (
    <SimStateContext.Provider value={sim}>
      <div className="app" data-density={t.density}>
        <div className="app-body" data-rail={t.railCollapsed ? 'collapsed' : 'expanded'}>
          <IconRail
            active={workspace}
            onPick={(id) => setTweak('workspace', id)}
            collapsed={t.railCollapsed}
            showAI={t.showAI}
          />
          <div className="workspace">
            <WorkspaceHeader
              title={meta.label}
              subtitle={subtitleFor(workspace)}
              tabs={tabsFor(workspace)}
              tools={
                <div style={{ display: 'inline-flex', gap: 4 }}>
                  <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Setup wizard" onClick={() => setOnboardingOpen(true)}><Icon name="sparkle" size={12}/></button>
                  <button className="btn btn-sm btn-ghost tt-wrap" data-tt="Notifications"><Icon name="bell" size={12}/></button>
                </div>
              }
            />
            <main style={{ minHeight: 0, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
              {workspace === 'dashboard' && <Dashboard/>}
              {workspace === 'record'    && <RecordingStudio/>}
              {workspace === 'stream'    && <StreamStudio/>}
              {workspace === 'editor'    && <VideoEditor/>}
              {workspace === 'clips'     && <ClipsLibrary/>}
              {workspace === 'media'     && <MediaLibrary/>}
              {workspace === 'projects'  && <ProjectsManager/>}
              {workspace === 'render'    && <RenderQueue/>}
              {workspace === 'settings'  && <SettingsScreen/>}
              {workspace === 'ai'        && t.showAI && <AILab/>}
            </main>
          </div>
        </div>
        <StatusBar/>
      </div>

      <Onboarding open={onboardingOpen} onClose={() => setOnboardingOpen(false)}/>

      {/* Tweaks panel */}
      <TweaksPanel title="Tweaks">
        <TweakSection label="App state"/>
        <TweakRadio  label="Mode" value={t.simMode} options={['idle','recording','streaming','rendering']} onChange={(v)=>setTweak('simMode', v)}/>
        <TweakSelect label="Workspace" value={t.workspace}
          options={NAV.filter(n => t.showAI || n.id !== 'ai').map(n => ({ value: n.id, label: n.label }))}
          onChange={(v)=>setTweak('workspace', v)}/>

        <TweakSection label="Layout"/>
        <TweakRadio  label="Density" value={t.density} options={['comfortable','compact','dense']} onChange={(v)=>setTweak('density', v)}/>
        <TweakToggle label="Collapse rail" value={t.railCollapsed} onChange={(v)=>setTweak('railCollapsed', v)}/>
        <TweakToggle label="Show AI Lab" value={t.showAI} onChange={(v)=>setTweak('showAI', v)}/>

        <TweakSection label="Theme"/>
        <TweakRadio  label="Background" value={t.bg} options={['true','charcoal','blue']} onChange={(v)=>setTweak('bg', v)}/>
        <TweakColor  label="Accent" value={t.accent}
          options={['amber','green','blue','violet','bone']}
          onChange={(v)=>setTweak('accent', v)}/>

        <TweakSection label="Misc"/>
        <TweakButton label="Launch onboarding" onClick={() => setOnboardingOpen(true)}/>
        <TweakButton label="Open codebase audit" onClick={() => window.open('audit.html', '_blank')} secondary/>
      </TweaksPanel>
    </SimStateContext.Provider>
  );
}

// The Tweaks helpers render swatch ribbons for raw color strings, not token names.
// Patch TweakColor so passing accent token IDs renders the correct accent colors.
// (We patch by wrapping — original is already on window.)
const ACCENT_COLORS = {
  amber: 'oklch(0.74 0.16 55)',
  green: 'oklch(0.72 0.16 145)',
  blue:  'oklch(0.70 0.18 260)',
  violet:'oklch(0.72 0.18 305)',
  bone:  'oklch(0.85 0.02 100)',
};

// Quick token-aware accent picker built from primitives (overrides default behavior).
const _TweakSection = window.TweakSection;
const _TweakColor = window.TweakColor;
window.TweakColor = function PatchedTweakColor({ label, value, options, onChange }) {
  // If options are token names, render swatches with matching colors
  if (Array.isArray(options) && options.every(o => typeof o === 'string' && ACCENT_COLORS[o])) {
    return (
      <div style={{ display: 'grid', gridTemplateColumns: '88px 1fr', alignItems: 'center', gap: 8, padding: '4px 12px', fontSize: 11.5 }}>
        <span style={{ color: '#29261b' }}>{label}</span>
        <div style={{ display: 'flex', gap: 4 }}>
          {options.map(o => (
            <button key={o} onClick={() => onChange(o)} title={o} style={{
              width: 22, height: 22, borderRadius: 6,
              background: ACCENT_COLORS[o],
              border: value === o ? '2px solid #29261b' : '1px solid rgba(0,0,0,0.18)',
              cursor: 'pointer',
            }}/>
          ))}
        </div>
      </div>
    );
  }
  return <_TweakColor label={label} value={value} options={options} onChange={onChange}/>;
};

ReactDOM.createRoot(document.getElementById('root')).render(<MsApp/>);
