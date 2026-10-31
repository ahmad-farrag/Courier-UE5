// Local test servers for Courier's live tests and screenshots.
// Socket.IO on :3456, mock OAuth 2.0 provider + API on :3457.
const http = require('http');
const crypto = require('crypto');
const { Server } = require('socket.io');

// ---------------- Socket.IO ----------------
const ioHttp = http.createServer();
const io = new Server(ioHttp, { cors: { origin: '*' } });
io.on('connection', (socket) => {
  console.log('[sio] connect', socket.id);
  socket.emit('welcome', { sid: socket.id, players: 2, server: 'SoccerRumble match server' });
  socket.on('echo', (...args) => socket.emit('echo', ...args));
  socket.on('add', (payload, ack) => { if (typeof ack === 'function') ack({ sum: (payload.a || 0) + (payload.b || 0) }); });
  socket.on('kick', (payload, ack) => {
    io.emit('ball', { x: payload && payload.power ? payload.power * 3 : 12, y: 4, by: socket.id.slice(0, 6) });
    if (typeof ack === 'function') ack({ ok: true, tick: Date.now() % 100000 });
  });
});
const match = io.of('/match');
match.use((socket, next) => {
  if (socket.handshake.auth && socket.handshake.auth.token === 'letmein') return next();
  next(new Error('unauthorized: bad or missing token'));
});
match.on('connection', (socket) => {
  socket.emit('score', { home: 2, away: 1, minute: 67 });
  let minute = 67;
  const timer = setInterval(() => { minute += 1; match.emit('clock', { minute }); }, 1500);
  socket.on('kick', (payload, ack) => {
    match.emit('ball', { x: (payload && payload.power ? payload.power : 3) * 9, y: 14, by: socket.id.slice(0, 6) });
    if (typeof ack === 'function') ack({ ok: true, shot: 'on target' });
  });
  socket.on('disconnect', () => clearInterval(timer));
});
ioHttp.listen(3456, () => console.log('Socket.IO on :3456'));

// ---------------- OAuth 2.0 mock ----------------
const clients = { game: 'secret', short: 'secret', web: '' };
const tokens = new Map();      // access token -> { client, grant, user, expires }
const refresh = new Map();     // refresh token -> { client, user }
const codes = new Map();       // code -> { client, challenge, redirect }
const issue = (client, grant, user) => {
  const access = 'at_' + crypto.randomBytes(12).toString('hex');
  const life = client === 'short' ? 1 : 3600;
  tokens.set(access, { client, grant, user, expires: Date.now() + life * 1000 });
  const body = { access_token: access, token_type: 'Bearer', expires_in: life, scope: 'scores' };
  if (grant !== 'client_credentials' || client === 'short') {
    const r = 'rt_' + crypto.randomBytes(12).toString('hex');
    refresh.set(r, { client, user });
    body.refresh_token = r;
  }
  console.log('[oauth] issued', grant, 'for', client, user || '');
  return body;
};
const b64url = (buf) => buf.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const send = (res, status, obj, headers = {}) => { res.writeHead(status, { 'Content-Type': 'application/json', ...headers }); res.end(JSON.stringify(obj)); };

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (req.method === 'GET' && url.pathname === '/authorize') {
    const code = 'code_' + crypto.randomBytes(8).toString('hex');
    codes.set(code, { client: url.searchParams.get('client_id'), challenge: url.searchParams.get('code_challenge'), redirect: url.searchParams.get('redirect_uri') });
    const target = new URL(url.searchParams.get('redirect_uri'));
    target.searchParams.set('code', code);
    target.searchParams.set('state', url.searchParams.get('state'));
    res.writeHead(302, { Location: target.toString() });
    return res.end();
  }
  if (req.method === 'POST' && url.pathname === '/token') {
    let raw = '';
    req.on('data', (c) => (raw += c));
    return req.on('end', () => {
      const p = new URLSearchParams(raw);
      let id = p.get('client_id'), secret = p.get('client_secret');
      const auth = req.headers.authorization || '';
      if (auth.startsWith('Basic ')) [id, secret] = Buffer.from(auth.slice(6), 'base64').toString().split(':');
      if (!(id in clients) || (clients[id] && clients[id] !== secret)) return send(res, 401, { error: 'invalid_client', error_description: 'Unknown client or wrong secret' });
      const grant = p.get('grant_type');
      if (grant === 'client_credentials') return send(res, 200, issue(id, grant));
      if (grant === 'password') {
        if (p.get('username') !== 'ada' || p.get('password') !== 'hunter2') return send(res, 400, { error: 'invalid_grant', error_description: 'Wrong username or password' });
        return send(res, 200, issue(id, grant, 'ada'));
      }
      if (grant === 'refresh_token') {
        const r = refresh.get(p.get('refresh_token'));
        if (!r) return send(res, 400, { error: 'invalid_grant', error_description: 'Unknown refresh token' });
        return send(res, 200, issue(r.client, grant, r.user));
      }
      if (grant === 'authorization_code') {
        const c = codes.get(p.get('code'));
        codes.delete(p.get('code'));
        if (!c) return send(res, 400, { error: 'invalid_grant', error_description: 'Unknown code' });
        if (c.challenge && b64url(crypto.createHash('sha256').update(p.get('code_verifier') || '').digest()) !== c.challenge)
          return send(res, 400, { error: 'invalid_grant', error_description: 'PKCE verifier does not match' });
        return send(res, 200, issue(id, grant, 'ada'));
      }
      send(res, 400, { error: 'unsupported_grant_type' });
    });
  }
  if (req.method === 'GET' && url.pathname === '/me') {
    const token = (req.headers.authorization || '').replace(/^Bearer /, '');
    const t = tokens.get(token);
    if (!t || t.expires < Date.now()) return send(res, 401, { error: 'invalid_token' });
    return send(res, 200, { client: t.client, grant: t.grant, user: t.user || null, scopes: ['scores'] });
  }
  if (url.pathname === '/login') {
    // Cookie demo: sets a session cookie, /profile reads it.
    return send(res, 200, { ok: true }, { 'Set-Cookie': ['session=' + crypto.randomBytes(6).toString('hex') + '; Path=/; HttpOnly; Max-Age=86400', 'theme=dark; Path=/'] });
  }
  if (url.pathname === '/profile') {
    return send(res, 200, { cookies: req.headers.cookie || '' });
  }
  send(res, 404, { error: 'not_found' });
}).listen(3457, () => console.log('OAuth mock on :3457'));
