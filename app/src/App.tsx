import { useState, useEffect, useCallback } from 'react'
import { invoke } from '@tauri-apps/api/core'

// Parse the first NDEF record from user-page memory (pages 4+)
function parseNdefContent(mem: Map<number, { data: string; error?: boolean }>): string | null {
  const bytes: number[] = []
  for (let p = 4; ; p++) {
    const e = mem.get(p)
    if (!e?.data || e.error) break
    for (const h of (e.data.match(/.{2}/g) ?? [])) bytes.push(parseInt(h, 16))
    if (bytes.length > 512) break
  }

  let i = 0
  while (i < bytes.length) {
    const tlvTag = bytes[i++]
    if (tlvTag === 0x00) continue
    if (tlvTag === 0xFE || tlvTag === undefined) break
    let tlvLen = bytes[i++] ?? 0
    if (tlvLen === 0xFF) { tlvLen = ((bytes[i++] ?? 0) << 8) | (bytes[i++] ?? 0) }
    if (tlvTag !== 0x03) { i += tlvLen; continue }

    const r     = bytes.slice(i, i + tlvLen)
    const flags = r[0] ?? 0
    const tnf   = flags & 0x07
    const sr    = !!(flags & 0x10)
    const il    = !!(flags & 0x08)
    const typeLen = r[1] ?? 0

    let off = 2
    let plLen: number
    if (sr) {
      plLen = r[off++] ?? 0
    } else {
      plLen = ((r[off]??0)<<24)|((r[off+1]??0)<<16)|((r[off+2]??0)<<8)|(r[off+3]??0)
      off += 4
    }
    if (il) off++

    const typeBytes    = r.slice(off, off + typeLen)
    const payloadBytes = r.slice(off + typeLen, off + typeLen + plLen)

    // URI record (TNF=1, type='U' 0x55)
    if (tnf === 1 && typeLen === 1 && typeBytes[0] === 0x55) {
      const code = payloadBytes[0] ?? 0
      const uri  = new TextDecoder().decode(new Uint8Array(payloadBytes.slice(1)))
      const prefixes: Record<number, string> = {
        0x01:'http://www.', 0x02:'https://www.',
        0x03:'http://',     0x04:'https://',
        0x05:'tel:',        0x06:'mailto:',
      }
      return (prefixes[code] ?? '') + uri
    }

    // Text record (TNF=1, type='T' 0x54)
    if (tnf === 1 && typeLen === 1 && typeBytes[0] === 0x54) {
      const langLen = (payloadBytes[0] ?? 0) & 0x3F
      return new TextDecoder().decode(new Uint8Array(payloadBytes.slice(1 + langLen)))
    }

    return `NDEF record (TNF=${tnf}, ${plLen} bytes)`
  }
  return null
}

interface TagInfo {
  uid:      string
  type:     string
  pages?:   number
  sectors?: number
}

interface MemEntry {
  data:   string
  error?: boolean
}

export default function App() {
  const [ports, setPortList]   = useState<string[]>([])
  const [selPort, setSelPort]  = useState('')
  const [connected, setConn]   = useState(false)
  const [hidOn, setHidOn]      = useState(true)
  const [tag, setTag]          = useState<TagInfo | null>(null)
  const [mem, setMem]          = useState<Map<number, MemEntry>>(new Map())
  const [log, setLog]          = useState<string[]>([])
  const [reading, setReading]  = useState(false)

  // write-page form
  const [wpPage, setWpPage]   = useState('')
  const [wpData, setWpData]   = useState('')

  // MIFARE auth form
  const [mfSec,  setMfSec]    = useState('')
  const [mfKey,  setMfKey]    = useState('FFFFFFFFFFFF')
  const [mfKT,   setMfKT]     = useState('A')

  // MIFARE write-block form
  const [wbBlock, setWbBlock] = useState('')
  const [wbData,  setWbData]  = useState('')

  // NDEF writer
  const [ndefUrl,     setNdefUrl]     = useState('')
  const [ndefWriting, setNdefWriting] = useState(false)

  const pushLog = useCallback((msg: string) =>
    setLog(p => [...p.slice(-149), msg]), [])

  const refreshPorts = useCallback(async () => {
    const p = await invoke<string[]>('list_ports')
    setPortList(p)
    if (p.length && !selPort) setSelPort(p[0])
  }, [selPort])

  useEffect(() => { refreshPorts() }, [])

  const handleLine = useCallback((raw: string) => {
    try {
      const d = JSON.parse(raw)
      switch (d.event) {
        case 'ready':
          pushLog(`Device ready (fw ${d.fw})`)
          break
        case 'tag':
          setTag({ uid: d.uid, type: d.type, pages: d.pages, sectors: d.sectors })
          setMem(new Map())
          pushLog(`Tag: ${d.uid}  (${d.type})`)
          setReading(true)
          invoke('send_command', { cmd: 'READ_ALL' }).catch(() => setReading(false))
          break
        case 'tag_removed':
          setTag(null)
          pushLog('Tag removed')
          break
        case 'read_done':
          setReading(false)
          pushLog('Read complete')
          break
        case 'ok':
          pushLog(`OK: ${d.msg ?? d.mode ?? ''}`)
          break
        case 'error':
          pushLog(`Error: ${d.msg}`)
          break
        case 'status':
          setHidOn(d.hid)
          pushLog(`Status: HID ${d.hid ? 'on' : 'off'}`)
          break
        case 'disconnected':
          setConn(false)
          setTag(null)
          pushLog('Disconnected')
          break
      }
    } catch {
      pushLog(raw)
    }
  }, [pushLog])

  // Poll for serial lines every 100 ms while connected.
  // Page/block updates are batched into a single setMem call per tick so the
  // table doesn't re-render 135 times during a full read.
  useEffect(() => {
    if (!connected) return
    const id = setInterval(async () => {
      try {
        const lines = await invoke<string[]>('drain_lines')
        if (!lines.length) return

        const memUpdates: [number, MemEntry][] = []

        for (const raw of lines) {
          try {
            const d = JSON.parse(raw)
            if (d.event === 'page') {
              memUpdates.push([d.page,  { data: d.data ?? '', error: !!d.error }])
            } else if (d.event === 'block') {
              memUpdates.push([d.block, { data: d.data ?? '' }])
            } else {
              handleLine(raw)
            }
          } catch {
            pushLog(raw)
          }
        }

        if (memUpdates.length) {
          setMem(prev => {
            const m = new Map(prev)
            for (const [k, v] of memUpdates) m.set(k, v)
            return m
          })
        }
      } catch { /* port closed */ }
    }, 100)
    return () => clearInterval(id)
  }, [connected, handleLine, pushLog])

  const cmd = useCallback(
    (c: string) => invoke('send_command', { cmd: c }).catch(e => pushLog(`Error: ${e}`)),
    [pushLog]
  )

  const connect = async () => {
    try {
      await invoke('connect', { portName: selPort })
      setConn(true)
      pushLog(`Connected to ${selPort}`)
      await invoke('send_command', { cmd: 'STATUS' })
    } catch (e) { pushLog(`Connect failed: ${e}`) }
  }

  const disconnect = async () => {
    await invoke('disconnect')
    setConn(false); setTag(null)
    pushLog('Disconnected')
  }

  const toggleHid = async () => {
    await cmd(hidOn ? 'MODE:APP' : 'MODE:HID')
    setHidOn(h => !h)
  }

  const readAll = async () => {
    setReading(true); setMem(new Map())
    await cmd('READ_ALL')
  }

  const writePage = async () => {
    if (!wpPage || wpData.length !== 8) { pushLog('Need page number and exactly 8 hex chars'); return }
    const p = parseInt(wpPage)
    if (isProtected(p)) { pushLog(`Page ${p} is protected — user pages are 4–${userPageLast}`); return }
    await cmd(`WRITE_PAGE:${wpPage}:${wpData.toUpperCase()}`)
  }

  const authSector = async () => {
    if (!mfSec) return
    await cmd(`AUTH:${mfSec}:${mfKT}:${mfKey.toUpperCase()}`)
  }

  const readBlock = (block: number) => cmd(`READ_BLOCK:${block}`)

  const writeBlock = async () => {
    if (!wbBlock || wbData.length !== 32) { pushLog('Need block number and exactly 32 hex chars'); return }
    await cmd(`WRITE_BLOCK:${wbBlock}:${wbData.toUpperCase()}`)
  }

  const isMifare = tag?.type === 'MIFARE_CLASSIC'
  const pages    = tag?.pages ?? 0

  // Last 5 pages of any NTAG/Ultralight are config registers
  const configStart  = pages > 0 ? pages - 5 : 0
  const isProtected  = (p: number) => p < 4 || (pages > 0 && p >= configStart)
  const userPageLast = configStart - 1  // last writable user page

  const writeNdef = async () => {
    const url = ndefUrl.trim()
    if (!url || !tag) return

    // Normalise Spotify: bare ID or spotify:playlist:ID → HTTPS URL
    let finalUrl = url
    const spotifyId = url.match(/^([A-Za-z0-9]{22})$/)
    if (spotifyId) finalUrl = `https://open.spotify.com/playlist/${spotifyId[1]}`
    const spotifyUri = url.match(/^spotify:(?:playlist|album|track):([A-Za-z0-9]+)$/)
    if (spotifyUri) finalUrl = `https://open.spotify.com/${url.split(':')[1]}/${spotifyUri[1]}`

    // NDEF URI record encoding
    const prefixes: [number, string][] = [
      [0x01, 'http://www.'], [0x02, 'https://www.'],
      [0x03, 'http://'],     [0x04, 'https://'],
      [0x05, 'tel:'],        [0x06, 'mailto:'],
    ]
    let code = 0x00, rest = finalUrl
    for (const [c, pfx] of prefixes) {
      if (finalUrl.startsWith(pfx)) { code = c; rest = finalUrl.slice(pfx.length); break }
    }
    const restBytes  = new TextEncoder().encode(rest)
    const payloadLen = 1 + restBytes.length
    const record     = new Uint8Array([0xD1, 0x01, payloadLen, 0x55, code, ...restBytes])
    const rl = record.length
    const tlv = rl < 255
      ? new Uint8Array([0x03, rl, ...record, 0xFE])
      : new Uint8Array([0x03, 0xFF, rl >> 8, rl & 0xFF, ...record, 0xFE])
    const padded = new Uint8Array(Math.ceil(tlv.length / 4) * 4)
    padded.set(tlv)

    const pagesNeeded = padded.length / 4
    if (4 + pagesNeeded - 1 > userPageLast) {
      pushLog(`URL too long — needs ${pagesNeeded} pages, only ${userPageLast - 3} available`)
      return
    }

    setNdefWriting(true)
    try {
      for (let i = 0; i < pagesNeeded; i++) {
        const hex = Array.from(padded.slice(i * 4, i * 4 + 4))
          .map(b => b.toString(16).padStart(2, '0')).join('').toUpperCase()
        await cmd(`WRITE_PAGE:${4 + i}:${hex}`)
      }
      pushLog(`NDEF written (${pagesNeeded} pages): ${finalUrl}`)
    } finally {
      setNdefWriting(false)
    }
  }

  const hexToAscii = (hex: string) =>
    ((hex ?? '').match(/.{2}/g) ?? [])
      .map(h => { const c = parseInt(h, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : '.' })
      .join('')

  // Capabilities derived from memory dump (available after Read all pages)
  const ccData   = mem.get(3)?.data   // page 3 = capability container
  const lockData = mem.get(2)?.data   // page 2, bytes 2-3 = static lock bits

  const ndefContent  = parseNdefContent(mem)
  const ndefCapable  = ccData ? (ccData.startsWith('E1') ? 'Yes' : 'No') : '—'
  const writeAccess  = ccData ? (ccData.slice(6, 8) === '00' ? 'Read / Write' : 'Read-only') : '—'
  const lockStatus   = lockData
    ? (lockData.slice(4) === '0000' ? 'Unlocked'
      : lockData.slice(4) === 'FFFF' ? 'Fully locked'
      : 'Partial')
    : '—'
  const userBytes    = isMifare ? 752
    : pages > 0 ? (configStart - 4) * 4
    : 0

  return (
    <div className="app">
      <header>
        <span className="logo">RFID Tool</span>
        <div className={`dot ${connected ? 'on' : 'off'}`} title={connected ? 'Connected' : 'Disconnected'} />
      </header>

      <div className="body">
        {/* ── sidebar ── */}
        <aside>
          <section>
            <label>Serial port</label>
            <div className="row">
              <select value={selPort} onChange={e => setSelPort(e.target.value)}>
                {ports.map(p => <option key={p}>{p}</option>)}
                {!ports.length && <option disabled>No ports found</option>}
              </select>
              <button onClick={refreshPorts} title="Refresh">↻</button>
            </div>
            {!connected
              ? <button className="primary" onClick={connect} disabled={!selPort}>Connect</button>
              : <button onClick={disconnect}>Disconnect</button>
            }
          </section>

          {connected && (
            <section>
              <label>HID keyboard</label>
              <button className={hidOn ? 'active' : ''} onClick={toggleHid}>
                {hidOn ? '⌨ HID: ON' : '⌨ HID: OFF'}
              </button>
              <p className="hint">{hidOn ? 'Types UID + Tab into active window' : 'Silent — programming mode'}</p>
            </section>
          )}

          {tag && (
            <section>
              <label>Tag</label>
              <div className="tag-card">
                <div className="uid">{tag.uid}</div>
                <span className="type-badge">{tag.type}</span>
                <table className="cap-table">
                  <tbody>
                    <tr><td>Memory</td><td>
                      {userBytes > 0 ? `${userBytes} B user` : '—'}
                      {tag.pages   ? ` · ${tag.pages} pages`   : ''}
                      {tag.sectors ? ` · ${tag.sectors} sectors` : ''}
                    </td></tr>
                    <tr><td>NDEF</td>    <td>{ndefCapable}</td></tr>
                    {ndefContent && <tr><td>URL</td><td className="ndef-url">{ndefContent}</td></tr>}
                    <tr><td>Access</td>  <td className={writeAccess === 'Read-only' ? 'cap-warn' : 'cap-ok'}>{writeAccess}</td></tr>
                    {!isMifare && <tr><td>Locks</td><td className={lockStatus !== 'Unlocked' && lockStatus !== '—' ? 'cap-warn' : ''}>{lockStatus}</td></tr>}
                  </tbody>
                </table>
                {ccData === undefined && <p className="hint" style={{marginTop:4}}>Read all pages to see capabilities</p>}
              </div>
            </section>
          )}

          <section className="log-wrap">
            <label>Log</label>
            <div className="log">
              {log.map((l, i) => <div key={i}>{l}</div>)}
            </div>
          </section>
        </aside>

        {/* ── main ── */}
        <main>
          {!tag && (
            <div className="empty">
              <div className="empty-icon">📡</div>
              <p>{connected ? 'Hold a tag near the reader' : 'Connect to a port to begin'}</p>
            </div>
          )}

          {/* NTAG / Ultralight panel */}
          {tag && !isMifare && (
            <div className="panel">
              <div className="toolbar">
                <button onClick={readAll} disabled={reading}>
                  {reading ? 'Reading…' : 'Re-read'}
                </button>
              </div>

              <div className="form-box">
                <h3>Write NDEF URL</h3>
                <div className="row">
                  <input
                    placeholder="https://… or spotify:playlist:ID or bare playlist ID"
                    value={ndefUrl}
                    onChange={e => setNdefUrl(e.target.value)}
                    style={{ flex: 1 }}
                  />
                  <button className="primary" onClick={writeNdef} disabled={ndefWriting || !ndefUrl.trim()}>
                    {ndefWriting ? 'Writing…' : 'Write NDEF'}
                  </button>
                </div>
                <p className="hint">Writes from page 4. Supports https://, http://, Spotify playlist URLs/URIs/IDs.</p>
              </div>

              <div className="form-box">
                <h3>Write raw page</h3>
                <div className="row">
                  <input placeholder="Page" value={wpPage} onChange={e => setWpPage(e.target.value)} style={{ width: 70 }} />
                  <input placeholder="8 hex chars  e.g. 48656C6C" value={wpData}
                         onChange={e => setWpData(e.target.value)} style={{ flex: 1, fontFamily: 'monospace' }}
                         maxLength={8} />
                  <button onClick={writePage}>Write</button>
                </div>
              </div>

              <table>
                <thead>
                  <tr><th>Page</th><th>Hex</th><th>ASCII</th></tr>
                </thead>
                <tbody>
                  {Array.from({ length: pages }, (_, i) => {
                    const e = mem.get(i)
                    const rowClass = isProtected(i) ? 'sys' : e?.error ? 'err' : ''
                    return (
                      <tr key={i} className={rowClass}>
                        <td className="num">{i}{isProtected(i) ? ' 🔒' : ''}</td>
                        <td className="hex">{e?.data ?? '—'}</td>
                        <td className="asc">{e?.data ? hexToAscii(e.data) : ''}</td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          )}

          {/* MIFARE Classic panel */}
          {tag && isMifare && (
            <div className="panel">
              <div className="form-box">
                <h3>Authenticate sector</h3>
                <div className="row">
                  <input placeholder="Sector 0–15" value={mfSec}
                         onChange={e => setMfSec(e.target.value)} style={{ width: 90 }} />
                  <select value={mfKT} onChange={e => setMfKT(e.target.value)}>
                    <option value="A">Key A</option>
                    <option value="B">Key B</option>
                  </select>
                  <input placeholder="12 hex chars" value={mfKey}
                         onChange={e => setMfKey(e.target.value)}
                         style={{ flex: 1, fontFamily: 'monospace' }} maxLength={12} />
                  <button onClick={authSector}>Auth</button>
                </div>
              </div>

              <div className="form-box">
                <h3>Write block</h3>
                <div className="row">
                  <input placeholder="Block 0–63" value={wbBlock}
                         onChange={e => setWbBlock(e.target.value)} style={{ width: 90 }} />
                  <input placeholder="32 hex chars (16 bytes)" value={wbData}
                         onChange={e => setWbData(e.target.value)}
                         style={{ flex: 1, fontFamily: 'monospace' }} maxLength={32} />
                  <button onClick={writeBlock}>Write</button>
                </div>
              </div>

              <table>
                <thead>
                  <tr><th>Sector</th><th>Block</th><th>Data (hex)</th><th></th></tr>
                </thead>
                <tbody>
                  {Array.from({ length: 16 }, (_, s) =>
                    Array.from({ length: 4 }, (_, b) => {
                      const block = s * 4 + b
                      const e = mem.get(block)
                      return (
                        <tr key={block} className={b === 3 ? 'trailer' : ''}>
                          {b === 0 && <td className="num" rowSpan={4}>S{s}</td>}
                          <td className="num">{block}</td>
                          <td className="hex">{e?.data ?? '—'}</td>
                          <td>
                            <button className="sm" onClick={() => readBlock(block)}>Read</button>
                          </td>
                        </tr>
                      )
                    })
                  )}
                </tbody>
              </table>
            </div>
          )}
        </main>
      </div>
    </div>
  )
}
