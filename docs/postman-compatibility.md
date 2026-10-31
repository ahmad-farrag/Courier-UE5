# Postman compatibility

Courier reads and writes Postman's JSON formats, so collections can move between Postman and Unreal in both directions.

## Collections (v2.0 and v2.1)

| Postman | Courier |
|---|---|
| `info.name`, `info.description` | Collection name and description |
| `variable[]` | Collection variables (numbers and bools become text) |
| Nested `item[]` folders | Folder paths such as `Users/Admin`; empty folders are kept |
| `request.method` | Verb; anything non-standard becomes a custom verb |
| `request.url` (string or object) | URL and query rows; disabled query rows stay disabled |
| `request.header[]` | Headers, with their enabled state |
| Body `raw` | JSON when the language is `json`, otherwise Raw with a content type from the language (`text`, `xml`, `html`, `javascript`) |
| Body `urlencoded` | x-www-form-urlencoded |
| Body `formdata` | form-data; `type: file` rows become file fields using `src` |
| Body `file` | Binary, using `file.src` |
| Body `graphql` | GraphQL query and variables |
| Auth `bearer`, `basic`, `apikey` | The same, in both the v2.1 array form and the v2.0 object form |
| Auth `noauth` | No auth, and stops inheritance |
| Auth `oauth2` | OAuth 2.0: `grant_type` (`client_credentials`, `password_credentials`, `authorization_code`, `authorization_code_with_pkce`; `implicit` becomes authorization code with PKCE), `accessTokenUrl`, `authUrl`, `clientId`, `clientSecret`, `scope`, `audience`, `username`, `password`, `redirect_uri`, `headerPrefix`, `client_authentication`, `accessToken` |
| Auth on folders and the collection | Inherited by requests that do not set their own, as in Postman |

## Environments

`name` and `values[]` (key, value, enabled) map to an environment. **Import** in the window tells collections and environments apart automatically (`FCourierPostman::IsEnvironmentJson`).

## What is not imported

| Postman feature | What happens |
|---|---|
| Pre-request and test scripts (`event[]`) | Ignored |
| OAuth 1.0, Digest, Hawk, AWS Signature, NTLM, Akamai EdgeGrid | Request imported without auth; a warning is logged to `LogCourier` |
| OAuth 2.0 device code, token refresh options, token-in-query | Not mapped; the request gets the OAuth 2.0 settings above |
| Saved example responses | Ignored |
| Cookies, certificates, proxy settings | Ignored |

## Export

Exports are v2.1 collections that Postman opens directly. Each request keeps its folder, headers, body, auth and query rows (disabled rows are written with `"disabled": true`).

WebSocket, Socket.IO and Server-Sent Events requests have no v2.1 equivalent. They are exported as GET items with an extra `courier` object:

```json
"courier": { "protocol": "SocketIO", "namespace": "/match", "auth": "{\"token\":\"{{token}}\"}", "timeout": 30 }
```

Postman ignores the unknown field; Courier reads it back, so a round trip keeps the protocol, sub-protocols, Socket.IO namespace and auth payload, timeout, and `"cookies": false` when a request opts out of the cookie jar. OAuth 2.0 settings are exported in Postman's own `oauth2` format, so Postman picks them up.

## Variables

`{{name}}` syntax is identical. These dynamic variables are supported: `{{$guid}}`, `{{$randomUUID}}`, `{{$timestamp}}`, `{{$isoTimestamp}}`, `{{$randomInt}}`. Other `$` variables stay as typed.
