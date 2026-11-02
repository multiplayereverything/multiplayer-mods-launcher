// The lab's stand-ins on loopback: a web server for the catalog, mods-info and downloads, the central server's room
// list, and friends that speak the relay protocol.
import { createReadStream, existsSync, statSync } from 'node:fs'
import { createServer } from 'node:http'
import net from 'node:net'
import { join, normalize } from 'node:path'

export function startServers(work, ports) {
  const www = join(work, 'www')
  const http = createServer((req, res) => {
    const file = normalize(join(www, decodeURIComponent(req.url.split('?')[0])))
    if (!file.startsWith(www) || !existsSync(file) || !statSync(file).isFile()) {
      res.writeHead(404).end()
      return
    }
    res.writeHead(200, { 'Content-Length': statSync(file).size })
    createReadStream(file).pipe(res)
  }).listen(ports.http, '127.0.0.1')

  // The room list every game asks for: one open room, and one full one with a password whose game has started.
  // The newer answer (code, players, max, locked, state, region, ping, version, name) goes to "RELAY|2|ROOMS|<game>",
  // the older one to "RELAY|1|LIST|<game>".
  const rooms = net.createServer((s) => {
    s.on('data', (d) => s.end(String(d).includes('|ROOMS|')
      ? 'ROOMS|ABCDE,1,4,0,lobby,EU,40,1,Test room;FULL1,4,4,1,ingame,US,80,1,Full room\0'
      : 'LIST|ABCDE,1,4,Test room;FULL1,4,4,Full room\0'))
    s.on('error', () => {})
  }).listen(ports.rooms, '127.0.0.1')

  return () => {
    http.close()
    rooms.close()
  }
}

// Connects to the built-in relay as the game ("H|name") or a friend ("J|name"); close() leaves.
export function relayClient(port, kind, name) {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, '127.0.0.1', () => {
      s.write(`${kind}|${name}\0`)
      resolve({ close: () => s.destroy() })
    })
    s.on('error', reject)
  })
}
