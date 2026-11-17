/** All geometry uses local metres (east/north); all time uses simulation seconds. */
export interface TaxiNode { id: string; x: number; y: number; lat: number; lon: number }
export interface TaxiEdge { id: string; from: string; to: string; length: number; name: string; kind: 'taxiway' | 'parking_position' | 'runway' }
export interface AirportLocation { id: string; name: string; nodeId: string | null; kind: 'stand' | 'runway' | 'holding_point'; x: number; y: number; lat?: number; lon?: number; source?: string }
export interface AirportSurface { kind: string; name?: string; points: [number, number][] }
export interface AirportData { nodes: TaxiNode[]; edges: TaxiEdge[]; locations: AirportLocation[]; surfaces: AirportSurface[]; metadata: Record<string, unknown> }
export interface GraphArc { to: string; edgeId: string; length: number }
export type OperatingConfiguration = 'east' | 'west';
export type MovementMode = 'departure' | 'arrival' | 'transfer';
export type MovementType = MovementMode;
export interface AircraftProfile { typeCode: string; wingspanMetres: number; lengthMetres?: number }
export interface AircraftRouteData {
  profile: AircraftProfile; status: 'compatible' | 'review'; ruleIds: string[]; sourceIds: string[];
  configuration: OperatingConfiguration; movement: MovementMode;
  advisories: { id: string; reason: string; sourceIds: string[] }[]; warnings: string[]; airportFlags: string[];
}
export interface RouteClearanceCheckpoint { id: string; ruleId: string; nodeId: string; nodeIndex: number; distance: number; kind: 'runway-crossing' | 'hold-short'; reason: string; sourceIds: string[] }
export interface RouteOperationalData {
  configuration: OperatingConfiguration; movement?: MovementMode; ruleIds: string[]; sourceIds: string[];
  unverifiedEdgeIds: string[]; clearanceCheckpoints: RouteClearanceCheckpoint[];
  advisories: { id: string; nodeId: string; reason: string; sourceIds: string[] }[];
}
export interface RouteFlowData {
  status: 'nominal' | 'review'; configuration: OperatingConfiguration; movement: MovementMode;
  policyIds: string[]; sourceIds: string[]; axes: string[]; requiredStages: string[];
  apronTransitMetres: number; mainAxisReentries: number; physicalDistance: number;
  significantTurns?: number; mainAxisChanges?: number; turnCostMetres?: number; routingCostMetres?: number;
  curvatureDegrees?: number; curvatureCostMetres?: number;
  originId?: string; destinationId?: string; warnings: string[];
}
export interface RouteAssignment { source: 'standard' | 'controller'; revision: number; waypointNodeIds: string[] }
export interface Route { nodeIds: string[]; edgeIds: string[]; distance: number; cumulativeDistances: number[]; operational?: RouteOperationalData; flow?: RouteFlowData; aircraft?: AircraftRouteData; assignment?: RouteAssignment }
export type AircraftState = 'scheduled' | 'taxiing' | 'holding' | 'arrived';
export interface AircraftObservation { edgeId: string; fromNodeId: string; toNodeId: string; edgeProgress: number }
export interface RouteDeviationEvent {
  id: string; aircraftId: string; callsign: string; time: number; reason: string;
  status: 'recovering' | 'unresolved' | 'rejoined'; observed: AircraftObservation;
  expectedEdgeId: string | null; rejoinNodeId?: string;
}
export interface RouteConformance {
  status: 'on-route' | 'recovering' | 'unresolved'; events: RouteDeviationEvent[]; assignedNodeIndex: number;
  rejoinNodeId?: string; rejoinDistance?: number; awaitingApproval?: boolean;
}
export interface Aircraft {
  id: string; callsign: string; originId: string; destinationId: string; route: Route;
  aircraftProfile?: AircraftProfile;
  assignedRoute?: Route; standardRoute?: Route; conformance?: RouteConformance; pendingWrongTurn?: boolean; scheduledAt?: number;
  distance: number; speed: number; state: AircraftState;
  x: number; y: number; heading: number; bodyHeading?: number; edgeId: string | null;
  fromNodeId: string; toNodeId: string; edgeProgress: number;
  waitingClearanceId?: string; clearedClearanceIds?: string[];
  yielding?: { ruleId: string; priorityAircraftId: string; nodeId: string; reason: string; sourceIds: string[]; stopDistance?: number };
}
export interface AircraftOptions { id?: string; callsign?: string; speed?: number; distance?: number; originId?: string; destinationId?: string; movement?: MovementMode; aircraftProfile?: AircraftProfile; scheduledAt?: number }
export interface EdgeOccupancy {
  edgeId: string; from: string; to: string; start: number; end: number;
  /** Offset and signed velocity use the edge's stored from → to orientation. */
  offset: number; velocity: number; stationary: boolean;
}
export interface NodeOccupancy { nodeId: string; start: number; end: number; time: number; incoming: string | null; outgoing: string | null; stationary: boolean }
export interface PredictedTrajectory { aircraft: Aircraft; edges: EdgeOccupancy[]; nodes: NodeOccupancy[]; horizon: number }
export type ConflictType = 'head-on' | 'crossing' | 'overtaking' | 'blockage';
export interface Conflict {
  id: string; type: ConflictType; severity: 'critical' | 'warning'; aircraftIds: [string, string];
  timeToConflict: number; x: number; y: number; location: string; message: string;
}
