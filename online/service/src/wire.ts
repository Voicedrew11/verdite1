// The protocol's constants and frames, shared by the router and the room.

export const PROTOCOL = 1;
export const ROOM_PEERS = 4;
export const FRAME_BYTES = 64 * 1024;
export const FRAMES_PER_SECOND = 50;

export type ErrorCode =
  | "full"
  | "build_mismatch"
  | "bad_message"
  | "unknown_peer"
  | "rate_limited"
  | "unavailable";

export type ServerFrame =
  | { type: "welcome"; protocol: number; peer: string; peers: string[]; iceServers: unknown[] }
  | { type: "join"; peer: string }
  | { type: "leave"; peer: string }
  | { type: "signal"; from: string; data: unknown }
  | { type: "error"; code: ErrorCode; message: string };

export function refusal(status: number, error: string, message: string): Response {
  return Response.json({ error, message }, { status });
}
