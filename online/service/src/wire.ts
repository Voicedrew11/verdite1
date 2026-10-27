// The protocol's constants and frames, shared by the router and the room.

export const PROTOCOL = 1;
export const ROOM_PEERS = 4;
export const FRAME_BYTES = 64 * 1024;
export const FRAMES_PER_SECOND = 50;
export const LISTED_ROOMS = 50;
export const PUBLIC_ROOM = /^P[A-HJ-NP-Z2-9]{7}$/;
export const HOST_NAME = /^[A-Za-z0-9 _-]{1,16}$/;
export const DESCRIBE_EVERY_MS = 1000;
export const FLOOR_MOST = 99;

export interface ListedRoom {
  room: string;
  host: string;
  floor: number;
  players: number;
  most: number;
}

export type ErrorCode =
  | "full"
  | "build_mismatch"
  | "bad_message"
  | "unknown_peer"
  | "rate_limited"
  | "alone"
  | "unavailable";

export type ServerFrame =
  | { type: "welcome"; protocol: number; peer: string; peers: string[] }
  | { type: "join"; peer: string }
  | { type: "leave"; peer: string }
  | { type: "signal"; from: string; data: unknown }
  | { type: "turn"; iceServers: unknown[]; seconds: number }
  | { type: "rooms"; rooms: ListedRoom[] }
  | { type: "error"; code: ErrorCode; message: string };

export function refusal(status: number, error: string, message: string): Response {
  return Response.json({ error, message }, { status });
}

// The lobby of a game's build: rooms are listed only for peers that can join them.
export function lobbyName(game: string, build: string): string {
  return `${game}/${build}`;
}
