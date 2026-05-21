// libraries.jsx — Clips Library, Media Library, Projects Manager screens.

// ─── Clips Library ───
function ClipsLibrary() {
  const [view, setView] = useState('grid');
  const [filter, setFilter] = useState('all');
  const filters = [
    { id: 'all', label: 'All clips', count: 42 },
    { id: 'fav', label: 'Favorites', count: 7 },
    { id: 'rec', label: 'Last session', count: 12 },
    { id: 'arc', label: 'Archived', count: 18 },
  ];
  const sources = [
    { label: 'Spire of the Hollow Sun', count: 18 },
    { label: 'Coding Sessions',         count: 9 },
    { label: 'Tuesday Vlog',            count: 7 },
    { label: 'Unsorted',                count: 8 },
  ];
  return (
    <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
      {/* Left filters */}
      <aside style={{ width: 240, borderRight: '1px solid var(--border)', background: 'var(--surface-0)', flex: '0 0 auto', overflow: 'auto' }}>
        <SectionH>Smart filters</SectionH>
        <div style={{ padding: '0 6px' }}>
          {filters.map(f => (
            <div key={f.id} className="row" data-active={filter === f.id} onClick={() => setFilter(f.id)} style={{ cursor: 'pointer' }}>
              <Icon name={f.id === 'fav' ? 'star' : f.id === 'arc' ? 'folder' : 'clips'} size={12} style={{ color: 'var(--text-mute)' }}/>
              <span style={{ fontSize: 'var(--fs-sm)' }}>{f.label}</span>
              <span className="row-meta">{f.count}</span>
            </div>
          ))}
        </div>
        <SectionH>Source</SectionH>
        <div style={{ padding: '0 6px' }}>
          {sources.map((s, i) => (
            <div key={i} className="row" style={{ cursor: 'pointer' }}>
              <Icon name="folder" size={12} style={{ color: 'var(--text-mute)' }}/>
              <span style={{ fontSize: 'var(--fs-sm)' }}>{s.label}</span>
              <span className="row-meta">{s.count}</span>
            </div>
          ))}
        </div>
        <SectionH>Tags</SectionH>
        <div style={{ padding: '0 12px 12px', display: 'flex', flexWrap: 'wrap', gap: 4 }}>
          {['no-hit', 'highlight', 'fail', 'banter', 'reveal', 'tutorial', 'reaction', 'b-roll'].map(t => (
            <span key={t} className="tag" style={{ cursor: 'pointer' }}>{t}</span>
          ))}
        </div>
      </aside>

      {/* Main */}
      <main style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', display: 'flex', gap: 8, alignItems: 'center' }}>
          <div style={{ position: 'relative', width: 320 }}>
            <Icon name="search" size={12} style={{ position: 'absolute', left: 8, top: '50%', transform: 'translateY(-50%)', color: 'var(--text-mute)' }}/>
            <input className="input" placeholder="Search clips, tags, project…" style={{ paddingLeft: 26 }}/>
          </div>
          <select className="input" style={{ width: 140 }}>
            <option>Newest first</option>
            <option>Oldest first</option>
            <option>Longest</option>
            <option>Shortest</option>
          </select>
          <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>42 clips · 3.2 GB</span>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
            <button className="btn btn-sm btn-ghost" onClick={() => setView('grid')} style={{ background: view==='grid' ? 'var(--surface-2)' : 'transparent' }}><Icon name="grid" size={12}/></button>
            <button className="btn btn-sm btn-ghost" onClick={() => setView('list')} style={{ background: view==='list' ? 'var(--surface-2)' : 'transparent' }}><Icon name="list" size={12}/></button>
            <button className="btn btn-sm"><Icon name="upload" size={12}/>Export selected</button>
          </div>
        </div>

        <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
          {view === 'grid' ? <ClipsGrid/> : <ClipsList/>}
        </div>
      </main>
    </div>
  );
}

const CLIPS_DATA = [
  { id: 1, t: 'No-hit boss phase 3', dur: '0:24', src: 'Spire', when: 'Today, 1h ago', tags: ['no-hit','highlight'], fav: true },
  { id: 2, t: 'Dodge into riposte', dur: '0:18', src: 'Spire', when: 'Today, 1h ago', tags: ['highlight'], fav: false },
  { id: 3, t: 'Boss death animation', dur: '0:42', src: 'Spire', when: 'Today, 2h ago', tags: ['reveal'], fav: true },
  { id: 4, t: 'Bit about coffee', dur: '0:09', src: 'Vlog', when: 'Yesterday', tags: ['banter'], fav: false },
  { id: 5, t: 'Refactor reveal', dur: '0:42', src: 'Coding', when: 'Yesterday', tags: ['reveal','tutorial'], fav: false },
  { id: 6, t: 'Death — corrupted run', dur: '0:11', src: 'Spire', when: '2 days', tags: ['fail'], fav: false },
  { id: 7, t: 'Reaction — first time seeing boss', dur: '0:31', src: 'Spire', when: '3 days', tags: ['reaction'], fav: true },
  { id: 8, t: 'Build tooling segue', dur: '0:08', src: 'Coding', when: '4 days', tags: ['b-roll'], fav: false },
  { id: 9, t: 'No-hit Phase 1 victory shout', dur: '0:14', src: 'Spire', when: '5 days', tags: ['highlight','reaction'], fav: false },
  { id: 10, t: 'Outro joke', dur: '0:06', src: 'Vlog', when: '6 days', tags: ['banter'], fav: false },
  { id: 11, t: 'Tutorial — vim binding', dur: '1:12', src: 'Coding', when: '1 week', tags: ['tutorial'], fav: true },
  { id: 12, t: 'Phase 2 first attempt', dur: '0:38', src: 'Spire', when: '1 week', tags: ['fail'], fav: false },
];

function ClipsGrid() {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: 14 }}>
      {CLIPS_DATA.map(c => (
        <div key={c.id} style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
          <div style={{ position: 'relative' }}>
            <Placeholder label={c.t} ratio="16/9" radius={6}/>
            <span className="tag mono" style={{ position: 'absolute', bottom: 6, right: 6, background: 'oklch(0 0 0 / 0.7)', borderColor: 'transparent', color: 'var(--text)' }}>{c.dur}</span>
            <span style={{ position: 'absolute', top: 6, right: 6, color: c.fav ? 'var(--warn)' : 'oklch(0.97 0 0 / 0.5)' }}>
              <Icon name="star" size={14}/>
            </span>
            <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', opacity: 0, transition: 'opacity 120ms', background: 'oklch(0 0 0 / 0.4)', borderRadius: 6 }}
                 onMouseEnter={(e) => e.currentTarget.style.opacity = 1}
                 onMouseLeave={(e) => e.currentTarget.style.opacity = 0}>
              <button className="btn" style={{ background: 'var(--accent)', borderColor: 'var(--accent)', color: 'oklch(0.1 0 0)' }}><Icon name="play" size={12}/>Preview</button>
            </div>
          </div>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 2 }}>
            <span style={{ fontSize: 12, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.t}</span>
            <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline' }}>
              <span className="mute mono" style={{ fontSize: 10 }}>{c.src} · {c.when}</span>
              <div style={{ display: 'inline-flex', gap: 4 }}>
                {c.tags.slice(0, 2).map(t => <span key={t} className="tag" style={{ fontSize: 9 }}>{t}</span>)}
              </div>
            </div>
          </div>
        </div>
      ))}
    </div>
  );
}

function ClipsList() {
  return (
    <div className="card" style={{ overflow: 'hidden' }}>
      <div style={{ display: 'grid', gridTemplateColumns: '40px 1fr 100px 120px 140px 100px 60px', alignItems: 'center', gap: 8, padding: '8px 12px', borderBottom: '1px solid var(--border)', background: 'var(--surface-1)', fontSize: 'var(--fs-xs)', color: 'var(--text-mute)', textTransform: 'uppercase', letterSpacing: '0.06em', fontWeight: 500 }}>
        <span></span><span>Name</span><span>Duration</span><span>Source</span><span>Tags</span><span>Recorded</span><span></span>
      </div>
      {CLIPS_DATA.map(c => (
        <div key={c.id} style={{ display: 'grid', gridTemplateColumns: '40px 1fr 100px 120px 140px 100px 60px', alignItems: 'center', gap: 8, padding: '8px 12px', borderBottom: '1px solid var(--border)' }}>
          <Placeholder label="" ratio="16/9" radius={3} style={{ width: 40 }}/>
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, minWidth: 0 }}>
            <Icon name="star" size={12} style={{ color: c.fav ? 'var(--warn)' : 'var(--text-faint)' }}/>
            <span style={{ fontSize: 'var(--fs-sm)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{c.t}</span>
          </div>
          <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>{c.dur}</span>
          <span style={{ fontSize: 'var(--fs-sm)', color: 'var(--text-dim)' }}>{c.src}</span>
          <span style={{ display: 'inline-flex', gap: 4 }}>{c.tags.slice(0,2).map(t => <span key={t} className="tag" style={{ fontSize: 9 }}>{t}</span>)}</span>
          <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{c.when}</span>
          <button className="btn btn-sm btn-ghost"><Icon name="more" size={12}/></button>
        </div>
      ))}
    </div>
  );
}

// ─── Media Library ───
function MediaLibrary() {
  return (
    <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
      <aside style={{ width: 240, borderRight: '1px solid var(--border)', background: 'var(--surface-0)', overflow: 'auto', flex: '0 0 auto' }}>
        <SectionH>Folders</SectionH>
        <div style={{ padding: '0 6px' }}>
          <FolderRow icon="folder" label="All media" count={284} active/>
          <FolderRow icon="folder" label="Footage"   count={142}/>
          <div style={{ paddingLeft: 16 }}>
            <FolderRow icon="folder" label="Spire — Ep 14" count={32}/>
            <FolderRow icon="folder" label="Coding — S3"   count={48}/>
            <FolderRow icon="folder" label="Vlog"          count={62}/>
          </div>
          <FolderRow icon="folder" label="Audio"      count={48}/>
          <FolderRow icon="folder" label="Images"     count={62}/>
          <FolderRow icon="folder" label="Projects"   count={32}/>
        </div>
        <SectionH>File types</SectionH>
        <div style={{ padding: '0 6px' }}>
          {[
            { label: 'Video',  ext: 'mp4 · mkv · mov', count: 142 },
            { label: 'Audio',  ext: 'wav · flac · mp3', count: 48 },
            { label: 'Image',  ext: 'png · jpg · psd',  count: 62 },
            { label: 'Project', ext: 'malloy',         count: 32 },
          ].map(t => (
            <div key={t.label} className="row" style={{ cursor: 'pointer' }}>
              <Icon name="media" size={12} style={{ color: 'var(--text-mute)' }}/>
              <div style={{ display: 'flex', flexDirection: 'column', minWidth: 0 }}>
                <span style={{ fontSize: 'var(--fs-sm)' }}>{t.label}</span>
                <span className="mute mono" style={{ fontSize: 10 }}>{t.ext}</span>
              </div>
              <span className="row-meta">{t.count}</span>
            </div>
          ))}
        </div>
      </aside>

      <main style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', display: 'flex', gap: 8, alignItems: 'center' }}>
          <div style={{ position: 'relative', width: 320 }}>
            <Icon name="search" size={12} style={{ position: 'absolute', left: 8, top: '50%', transform: 'translateY(-50%)', color: 'var(--text-mute)' }}/>
            <input className="input" placeholder="Search media…" style={{ paddingLeft: 26 }}/>
          </div>
          <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>284 files · 412 GB</span>
          <span className="tag tag-warn" style={{ marginLeft: 8 }}><Icon name="alert" size={10}/> 1 missing</span>
          <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
            <button className="btn btn-sm btn-ghost"><Icon name="link" size={12}/>Relink missing…</button>
            <button className="btn btn-sm"><Icon name="upload" size={12}/>Import</button>
          </div>
        </div>

        {/* Missing media banner */}
        <div style={{ padding: '10px 16px', background: 'oklch(0.80 0.15 75 / 0.08)', borderBottom: '1px solid oklch(0.80 0.15 75 / 0.4)', display: 'flex', alignItems: 'center', gap: 8 }}>
          <Icon name="alert" size={14} style={{ color: 'var(--warn)' }}/>
          <span style={{ fontSize: 'var(--fs-sm)' }}>One file is missing. <span className="mono mute">Thumbnail — final.psd</span> referenced by 1 project.</span>
          <button className="btn btn-sm" style={{ marginLeft: 'auto' }}>Locate…</button>
          <button className="btn btn-sm btn-ghost">Skip</button>
        </div>

        <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
          <MediaTable/>
        </div>
      </main>
    </div>
  );
}

function FolderRow({ icon, label, count, active }) {
  return (
    <div className="row" data-active={active} style={{ cursor: 'pointer' }}>
      <Icon name={icon} size={12} style={{ color: 'var(--text-mute)' }}/>
      <span style={{ fontSize: 'var(--fs-sm)' }}>{label}</span>
      <span className="row-meta">{count}</span>
    </div>
  );
}

const MEDIA = [
  { name: 'Spire — Ep 14 raw.mkv',     k: 'video', dim: '1920×1080 60p',  size: '14.2 GB',  used: true, when: '1h ago' },
  { name: 'Webcam α7C — Ep 14.mkv',     k: 'video', dim: '1920×1080 60p',  size: '4.8 GB',   used: true, when: '1h ago' },
  { name: 'Mic — SM7B (cleaned).wav',  k: 'audio', dim: '48 kHz · Stereo', size: '1.1 GB',   used: true, when: '45 min ago' },
  { name: 'Music — Synth bed.flac',     k: 'audio', dim: '48 kHz · Stereo', size: '380 MB',   used: true, when: 'Mar 04' },
  { name: 'Intro card v3.mp4',           k: 'video', dim: '3840×2160 60p',  size: '92 MB',    used: true, when: 'Feb 28' },
  { name: 'Lower-third — title.png',     k: 'image', dim: '1920×240',       size: '120 KB',   used: true, when: 'Feb 24' },
  { name: 'B-roll — sunset.mp4',         k: 'video', dim: '3840×2160 30p',  size: '220 MB',   used: false, when: 'Feb 12' },
  { name: 'SFX — whoosh 02.wav',         k: 'audio', dim: '48 kHz · Stereo', size: '220 KB',  used: false, when: 'Feb 10' },
  { name: 'Thumbnail — final.psd',       k: 'image', dim: '1920×1080',      size: '24 MB',    missing: true, when: 'Feb 02' },
];

function MediaTable() {
  return (
    <div className="card" style={{ overflow: 'hidden' }}>
      <div style={{ display: 'grid', gridTemplateColumns: '40px 1.6fr 1fr 100px 80px 100px 60px', alignItems: 'center', gap: 8, padding: '8px 12px', borderBottom: '1px solid var(--border)', background: 'var(--surface-1)', fontSize: 'var(--fs-xs)', color: 'var(--text-mute)', textTransform: 'uppercase', letterSpacing: '0.06em', fontWeight: 500 }}>
        <span></span><span>Name</span><span>Properties</span><span>Size</span><span>Used</span><span>Modified</span><span></span>
      </div>
      {MEDIA.map((m, i) => (
        <div key={i} style={{ display: 'grid', gridTemplateColumns: '40px 1.6fr 1fr 100px 80px 100px 60px', alignItems: 'center', gap: 8, padding: '8px 12px', borderBottom: '1px solid var(--border)', color: m.missing ? 'var(--warn)' : 'var(--text)' }}>
          <span style={{ width: 32, height: 18, background: 'var(--surface-2)', borderRadius: 3, display: 'inline-flex', alignItems: 'center', justifyContent: 'center', color: 'var(--text-mute)' }}>
            <Icon name={m.k === 'video' ? 'editor' : m.k === 'audio' ? 'speaker' : 'image'} size={10}/>
          </span>
          <span style={{ fontSize: 'var(--fs-sm)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{m.name}</span>
          <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>{m.dim}</span>
          <span className="mono mute" style={{ fontSize: 'var(--fs-xs)' }}>{m.size}</span>
          <span>{m.missing ? <span className="tag tag-warn">missing</span> : m.used ? <span className="dot-ok" style={{ display: 'inline-block', verticalAlign: 'middle' }}/> : <span className="tag" style={{ fontSize: 9 }}>unused</span>}</span>
          <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{m.when}</span>
          <button className="btn btn-sm btn-ghost"><Icon name="more" size={12}/></button>
        </div>
      ))}
    </div>
  );
}

// ─── Projects ───
function ProjectsManager() {
  const projects = [
    { name: 'Spire of the Hollow Sun', sub: 'Stream series · 14 eps', cover: 'Gameplay · Spire', meta: '142 GB · 14 sessions', state: 'Open', date: '2 hr ago', accent: true },
    { name: 'Tuesday Vlog — Week 22',  sub: 'Vlog · weekly',           cover: 'Webcam · A-roll', meta: '38 GB · 7 sessions',  state: 'Active', date: 'Yesterday' },
    { name: 'Coding Sessions · S3',    sub: 'Coding series · 6 eps',   cover: 'Editor capture',  meta: '88 GB · 6 sessions',  state: 'Draft',  date: '4 days' },
    { name: 'Boss Rush — Compilation', sub: 'One-off · 30 min',        cover: 'Final cut · v3',  meta: '21 GB · 1 session',   state: 'Final',  date: '2 weeks' },
    { name: 'Game Dev Diary · Pilot',  sub: 'Pilot · 12 min',           cover: 'Concept · placeholder', meta: '4 GB · 2 sessions', state: 'Draft', date: '1 month' },
    { name: 'Late-night Tutorial #07', sub: 'Tutorial · 22 min',        cover: 'Talking head', meta: '12 GB · 3 sessions', state: 'Archived', date: '3 months', archived: true },
  ];
  return (
    <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
      <div style={{ padding: '12px 16px', borderBottom: '1px solid var(--border)', display: 'flex', gap: 8, alignItems: 'center' }}>
        <div style={{ position: 'relative', width: 320 }}>
          <Icon name="search" size={12} style={{ position: 'absolute', left: 8, top: '50%', transform: 'translateY(-50%)', color: 'var(--text-mute)' }}/>
          <input className="input" placeholder="Search projects…" style={{ paddingLeft: 26 }}/>
        </div>
        <div style={{ display: 'flex', gap: 2, background: 'var(--surface-1)', border: '1px solid var(--border)', borderRadius: 'var(--r-2)', padding: 2 }}>
          {['All', 'Active', 'Drafts', 'Final', 'Archived'].map((t, i) => (
            <button key={t} className="btn btn-sm" style={{ height: 22, background: i === 0 ? 'var(--surface-2)' : 'transparent', borderColor: 'transparent' }}>{t}</button>
          ))}
        </div>
        <span className="mute mono" style={{ fontSize: 'var(--fs-xs)' }}>6 projects · 305 GB</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
          <button className="btn btn-sm btn-ghost"><Icon name="folder" size={12}/>Import folder</button>
          <button className="btn btn-sm btn-primary"><Icon name="plus" size={12}/>New project</button>
        </div>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 16 }}>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: 16 }}>
          {projects.map((p, i) => (
            <div key={i} className="card" style={{ padding: 12, opacity: p.archived ? 0.55 : 1 }}>
              <Placeholder label={p.cover} ratio="16/9" radius={6}/>
              <div style={{ display: 'flex', alignItems: 'flex-start', gap: 8, marginTop: 10 }}>
                <div style={{ minWidth: 0, flex: 1 }}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span style={{ fontSize: 'var(--fs-md)', fontWeight: 600 }}>{p.name}</span>
                    {p.accent && <span className="tag tag-accent">Current</span>}
                  </div>
                  <span className="mute" style={{ fontSize: 'var(--fs-xs)' }}>{p.sub}</span>
                </div>
                <button className="btn btn-sm btn-ghost"><Icon name="more" size={12}/></button>
              </div>
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginTop: 6 }}>
                <span className="mute mono" style={{ fontSize: 10 }}>{p.meta}</span>
                <span className="mute" style={{ fontSize: 10 }}>{p.date}</span>
              </div>
              <div style={{ display: 'flex', gap: 4, marginTop: 10 }}>
                <button className="btn btn-sm" style={{ flex: 1 }}><Icon name="play" size={11}/>Open</button>
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

Object.assign(window, { ClipsLibrary, MediaLibrary, ProjectsManager });
