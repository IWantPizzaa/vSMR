import type { Aircraft, AirportData, Conflict, Route } from './core/types';
import type { TaxiGraph } from './core/TaxiGraph';
import { footprintPolygons } from './core/AircraftGeometry';

const colors: Record<string, string> = {
  runway: '#39454a', taxiway: '#26353b', apron: '#243139',
  building: '#425055', terminal: '#425055', grass: '#182a28',
  terrain: '#1b292c', stopbar: '#d2746490', closure: '#d5b36c70',
};
type ExtendedData = AirportData & {
  labels?: { name?: string; text?: string; x: number; y: number; kind?: string }[];
  referenceLines?: { kind: string; name?: string; points: [number, number][] }[];
};
export interface RouteEditHandle { aircraftId: string; nodeIndex: number; nodeId: string; x: number; y: number; screenX: number; screenY: number }

/** Canvas only presents source geometry; it never modifies routing topology. */
export class AirportMap {
  private ctx: CanvasRenderingContext2D;
  private base = document.createElement('canvas');
  private baseCtx = this.base.getContext('2d')!;
  private width = 0;
  private height = 0;
  private dpr = 1;
  private scale = 0.1;
  private center = { x: 0, y: 0 };
  private dirty = true;
  private dragging: { x: number; y: number; moved: boolean } | null = null;
  private routeDragging: { handle: RouteEditHandle; x: number; y: number; targetNodeId: string | null; moved: boolean } | null = null;
  showStands = true;
  showLabels = true;
  showNetwork = true;
  showOperating = true;
  showTurnRestrictions = true;
  readonly restrictedTurnNodes = new Set<string>();
  showAxes = true;
  primaryAxes: { name: string; edgeIds: string[]; label?: { x: number; y: number } }[] = [];
  /** These arrows describe the selected configuration/movement only. */
  readonly edgeDirections = new Map<string, 'forward' | 'reverse' | 'closed'>();
  readonly closedLocationIds = new Set<string>();
  showReferences = true;
  showComponents = false;
  showPrediction = true;
  showFootprints = true;
  aircraft: Aircraft[] = [];
  conflicts: Conflict[] = [];
  route: Route | null = null;
  selectedId: string | null = null;
  onSelect: (id: string) => void = () => {};
  onPointer: (x: number, y: number) => void = () => {};
  onRouteEdit: (aircraftId: string, nodeIndex: number, targetNodeId: string, anchorNodeId: string) => void = () => {};

  constructor(private canvas: HTMLCanvasElement, private data: AirportData, private graph: TaxiGraph) {
    this.ctx = canvas.getContext('2d')!;
    new ResizeObserver(() => this.resize()).observe(canvas);
    canvas.addEventListener('wheel', e => {
      e.preventDefault();
      const rect = canvas.getBoundingClientRect();
      this.zoom(Math.exp(-e.deltaY * .0015), e.clientX - rect.left, e.clientY - rect.top);
    }, { passive: false });
    canvas.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      const rect = canvas.getBoundingClientRect(), x = e.clientX - rect.left, y = e.clientY - rect.top;
      const handle = this.getRouteEditHandles().map(handle => ({ handle, distance: Math.hypot(handle.screenX - x, handle.screenY - y) })).filter(item => item.distance <= 11).sort((a, b) => a.distance - b.distance)[0]?.handle;
      if (handle) {
        this.routeDragging = { handle, x: handle.x, y: handle.y, targetNodeId: handle.nodeId, moved: false };
        this.dragging = null; canvas.setPointerCapture(e.pointerId); return;
      }
      this.dragging = { x: e.clientX, y: e.clientY, moved: false };
      canvas.setPointerCapture(e.pointerId);
    });
    canvas.addEventListener('pointermove', e => {
      const rect = canvas.getBoundingClientRect();
      const x = e.clientX - rect.left, y = e.clientY - rect.top;
      this.onPointer(this.center.x + (x - this.width / 2) / this.scale, this.center.y - (y - this.height / 2) / this.scale);
      if (this.routeDragging) {
        const point = { x: this.center.x + (x - this.width / 2) / this.scale, y: this.center.y - (y - this.height / 2) / this.scale };
        const radius = Math.min(18 / this.scale, 60);
        let target: string | null = null, best = radius;
        for (const node of this.graph.nodes.values()) {
          if (!this.isEditableJunction(node.id)) continue;
          const distance = Math.hypot(node.x - point.x, node.y - point.y);
          if (distance < best) { best = distance; target = node.id; }
        }
        this.routeDragging.x = point.x; this.routeDragging.y = point.y; this.routeDragging.targetNodeId = target;
        if (Math.hypot(point.x - this.routeDragging.handle.x, point.y - this.routeDragging.handle.y) * this.scale > 3) this.routeDragging.moved = true;
        return;
      }
      if (!this.dragging) return;
      const dx = e.clientX - this.dragging.x, dy = e.clientY - this.dragging.y;
      if (Math.abs(dx) + Math.abs(dy) > 2) this.dragging.moved = true;
      this.center.x -= dx / this.scale; this.center.y += dy / this.scale;
      this.dragging.x = e.clientX; this.dragging.y = e.clientY;
      this.invalidate();
    });
    canvas.addEventListener('pointerup', e => {
      if (this.routeDragging) {
        const drag = this.routeDragging; this.routeDragging = null;
        if (drag.moved && drag.targetNodeId && drag.targetNodeId !== drag.handle.nodeId) this.onRouteEdit(drag.handle.aircraftId, drag.handle.nodeIndex, drag.targetNodeId, drag.handle.nodeId);
        return;
      }
      if (this.dragging && !this.dragging.moved) {
        const rect = canvas.getBoundingClientRect();
        let closest: Aircraft | null = null, best = 24;
        for (const a of this.aircraft) {
          if (a.state === 'scheduled') continue;
          const p = this.project(a.x, a.y);
          const d = Math.hypot(p.x - e.clientX + rect.left, p.y - e.clientY + rect.top);
          if (d < best) { best = d; closest = a; }
        }
        if (closest) this.onSelect(closest.id);
      }
      this.dragging = null;
    });
    canvas.addEventListener('pointercancel', () => { this.dragging = null; this.routeDragging = null; });
    canvas.addEventListener('dblclick', () => this.fit());
    canvas.addEventListener('keydown', e => {
      if (e.key === 'Escape') { this.routeDragging = null; this.dragging = null; }
      if (e.key === '+' || e.key === '=') this.zoom(1.3);
      if (e.key === '-') this.zoom(1 / 1.3);
      if (e.key.toLowerCase() === 'f') this.fit();
      if (['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(e.key)) {
        e.preventDefault();
        this.center.x += (e.key === 'ArrowRight' ? 80 : e.key === 'ArrowLeft' ? -80 : 0) / this.scale;
        this.center.y += (e.key === 'ArrowUp' ? 80 : e.key === 'ArrowDown' ? -80 : 0) / this.scale;
        this.invalidate();
      }
    });
    this.resize(); this.fit();
  }

  private isEditableJunction(nodeId: string): boolean {
    const edges = (this.graph.adjacency.get(nodeId) || []).map(arc => this.graph.edges.get(arc.edgeId)!).filter(edge => edge.kind === 'taxiway');
    return edges.length >= 3 || edges.length === 2 && new Set(edges.map(edge => edge.name).filter(Boolean)).size >= 2;
  }

  getRouteEditHandles(): RouteEditHandle[] {
    const aircraft = this.aircraft.find(a => a.id === this.selectedId);
    if (!aircraft || aircraft.state === 'arrived' || aircraft.conformance?.status === 'unresolved') return [];
    const handles: RouteEditHandle[] = [];
    aircraft.route.nodeIds.forEach((nodeId, nodeIndex) => {
      if (nodeIndex === 0 || nodeIndex === aircraft.route.nodeIds.length - 1 || aircraft.route.cumulativeDistances[nodeIndex] <= aircraft.distance + 1 || !this.isEditableJunction(nodeId)) return;
      const node = this.graph.nodes.get(nodeId)!, point = this.project(node.x, node.y);
      if (point.x < -10 || point.y < -10 || point.x > this.width + 10 || point.y > this.height + 10 || handles.some(handle => Math.hypot(handle.screenX - point.x, handle.screenY - point.y) < 16)) return;
      handles.push({ aircraftId: aircraft.id, nodeIndex, nodeId, x: node.x, y: node.y, screenX: point.x, screenY: point.y });
    });
    return handles;
  }

  private resize() {
    const r = this.canvas.getBoundingClientRect();
    this.width = r.width; this.height = r.height; this.dpr = Math.min(window.devicePixelRatio || 1, 2);
    this.canvas.width = this.base.width = Math.round(this.width * this.dpr);
    this.canvas.height = this.base.height = Math.round(this.height * this.dpr);
    this.invalidate();
  }

  fit() {
    const bounds = { minX: Infinity, maxX: -Infinity, minY: Infinity, maxY: -Infinity };
    const include = (x: number, y: number) => {
      bounds.minX = Math.min(bounds.minX, x); bounds.maxX = Math.max(bounds.maxX, x);
      bounds.minY = Math.min(bounds.minY, y); bounds.maxY = Math.max(bounds.maxY, y);
    };
    this.data.nodes.forEach(n => include(n.x, n.y));
    this.data.locations.forEach(l => include(l.x, l.y));
    this.data.surfaces.forEach(s => s.points.forEach(([x, y]) => include(x, y)));
    (this.data as ExtendedData).referenceLines?.forEach(l => l.points.forEach(([x, y]) => include(x, y)));
    if (!Number.isFinite(bounds.minX)) return;
    const { minX, maxX, minY, maxY } = bounds;
    this.center = { x: (minX + maxX) / 2, y: (minY + maxY) / 2 };
    this.scale = Math.min((this.width - 90) / (maxX - minX), (this.height - 150) / (maxY - minY));
    this.invalidate();
  }

  zoom(factor: number, anchorX = this.width / 2, anchorY = this.height / 2) {
    const oldScale = this.scale;
    this.scale = Math.max(.025, Math.min(4, this.scale * factor));
    this.center.x += (anchorX - this.width / 2) * (1 / oldScale - 1 / this.scale);
    this.center.y -= (anchorY - this.height / 2) * (1 / oldScale - 1 / this.scale);
    this.invalidate();
  }

  focus(x: number, y: number) { this.center = { x, y }; this.scale = Math.max(this.scale, .35); this.invalidate(); }
  invalidate() { this.dirty = true; }
  private project(x: number, y: number) { return { x: (x - this.center.x) * this.scale + this.width / 2, y: (this.center.y - y) * this.scale + this.height / 2 }; }
  private path(ctx: CanvasRenderingContext2D, points: { x: number; y: number }[], close = false) {
    if (!points.length) return;
    ctx.beginPath();
    points.forEach((p, i) => { const s = this.project(p.x, p.y); if (i) ctx.lineTo(s.x, s.y); else ctx.moveTo(s.x, s.y); });
    if (close) ctx.closePath();
  }

  private drawBase() {
    const c = this.baseCtx;
    c.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    c.fillStyle = '#182329'; c.fillRect(0, 0, this.width, this.height);
    // Subtle metric grid remains fixed to the geographic projection while panning.
    c.strokeStyle = '#ffffff05'; c.lineWidth = 1;
    const spacing = this.scale < .15 ? 1000 : 250;
    const left = this.center.x - this.width / 2 / this.scale;
    const bottom = this.center.y - this.height / 2 / this.scale;
    c.beginPath();
    for (let x = Math.floor(left / spacing) * spacing; x < left + this.width / this.scale; x += spacing) {
      const p = this.project(x, 0); c.moveTo(p.x, 0); c.lineTo(p.x, this.height);
    }
    for (let y = Math.floor(bottom / spacing) * spacing; y < bottom + this.height / this.scale; y += spacing) {
      const p = this.project(0, y); c.moveTo(0, p.y); c.lineTo(this.width, p.y);
    }
    c.stroke();
    for (const surface of this.data.surfaces) {
      this.path(c, surface.points.map(([x, y]) => ({ x, y })), true);
      c.fillStyle = colors[surface.kind.toLowerCase()] || '#28363c'; c.fill();
      if (/building|terminal/.test(surface.kind.toLowerCase())) { c.strokeStyle = '#68777b55'; c.lineWidth = .7; c.stroke(); }
    }
    if (this.showReferences) {
      for (const line of (this.data as ExtendedData).referenceLines || []) {
        this.path(c, line.points.map(([x, y]) => ({ x, y })));
        c.strokeStyle = line.kind === 'excluded' ? '#f3a285bb' : '#8db0bd33';
        c.lineWidth = line.kind === 'excluded' ? 2 : .65;
        c.setLineDash(line.kind === 'excluded' ? [3, 3] : []); c.stroke();
      }
      c.setLineDash([]);
    }
    if (this.showNetwork) {
      c.lineWidth = Math.max(.7, Math.min(1.5, this.scale * 4));
      for (const edge of this.data.edges) {
        const a = this.graph.nodes.get(edge.from), b = this.graph.nodes.get(edge.to);
        if (!a || !b) continue;
        const component = this.graph.componentByNode.get(edge.from) ?? 0;
        c.strokeStyle = this.showComponents ? `hsl(${(Number(component) * 67 + 95) % 360} 65% 62% / 75%)` : edge.kind === 'runway' ? '#d0d9da55' : edge.kind === 'parking_position' ? '#9baa7355' : '#9eae8777';
        this.path(c, [a, b]); c.stroke();
      }
    }
    if (this.showAxes) {
      for (const axis of this.primaryAxes) {
        c.strokeStyle = '#70a7bd55'; c.lineWidth = 3.5;
        for (const id of axis.edgeIds) {
          const edge = this.graph.edges.get(id); if (!edge) continue;
          this.path(c, [this.graph.nodes.get(edge.from)!, this.graph.nodes.get(edge.to)!]); c.stroke();
        }
      }
    }
    if (this.showOperating) {
      for (const [id, direction] of this.edgeDirections) {
        const edge = this.graph.edges.get(id);
        if (!edge) continue;
        const a = this.graph.nodes.get(edge.from)!, b = this.graph.nodes.get(edge.to)!;
        if (direction === 'closed') {
          this.path(c, [a, b]); c.strokeStyle = '#f1a077aa'; c.lineWidth = 2;
          c.setLineDash([3, 4]); c.stroke(); c.setLineDash([]); continue;
        }
        if (edge.length * this.scale < 12) continue;
        const p = this.project((a.x + b.x) / 2, (a.y + b.y) / 2);
        const heading = -Math.atan2(b.y - a.y, b.x - a.x) + (direction === 'reverse' ? Math.PI : 0);
        c.save(); c.translate(p.x, p.y); c.rotate(heading);
        c.beginPath(); c.moveTo(-3, -2.5); c.lineTo(2, 0); c.lineTo(-3, 2.5);
        c.strokeStyle = '#c3df9baa'; c.lineWidth = 1.1; c.stroke(); c.restore();
      }
    }
    if (this.showTurnRestrictions && this.scale > .16) {
      c.strokeStyle = '#efa476'; c.lineWidth = 1.3;
      for (const id of this.restrictedTurnNodes) {
        const node = this.graph.nodes.get(id); if (!node) continue;
        const p = this.project(node.x, node.y);
        if (p.x < -8 || p.y < -8 || p.x > this.width + 8 || p.y > this.height + 8) continue;
        c.beginPath(); c.arc(p.x, p.y, 4.5, 0, Math.PI * 2); c.stroke();
        c.beginPath(); c.moveTo(p.x - 3, p.y + 3); c.lineTo(p.x + 3, p.y - 3); c.stroke();
      }
    }
    if (this.showStands && this.scale > .09) {
      for (const loc of this.data.locations.filter(l => l.kind === 'stand')) {
        const p = this.project(loc.x, loc.y);
        const connected = loc.nodeId && this.graph.componentByNode.get(loc.nodeId) === 0 && !this.closedLocationIds.has(loc.id);
        c.fillStyle = connected ? '#7d978c' : '#d99760';
        c.fillRect(p.x - 1, p.y - 1, 2, 2);
        if (this.showLabels && this.scale > .45) {
          c.font = '9px ui-monospace, monospace'; c.fillStyle = connected ? '#a9b7af' : '#eeb580';
          c.fillText(loc.name, p.x + 4, p.y + 3);
        }
      }
    }
    if (this.showLabels) {
      const extended = this.data as ExtendedData;
      const occupied = new Set<string>();
      for (const label of extended.labels || []) {
        if (label.kind === 'stand' || label.kind === 'gate') continue;
        if (this.scale < .28 && !/runway|terminal/.test(label.kind || '')) continue;
        const p = this.project(label.x, label.y);
        const key = `${Math.round(p.x / 48)},${Math.round(p.y / 20)}`;
        if (occupied.has(key)) continue; occupied.add(key);
        c.font = '10px ui-monospace, monospace'; c.fillStyle = '#aab5b6'; c.fillText(label.name || label.text || '', p.x, p.y);
      }
      for (const loc of this.data.locations.filter(l => l.kind === 'runway')) {
        const p = this.project(loc.x, loc.y);
        c.font = 'bold 11px ui-monospace, monospace';
        const labelWidth = c.measureText(loc.name).width;
        c.fillStyle = '#121c21dc'; c.fillRect(p.x - 5, p.y - 17, labelWidth + 10, 23);
        c.fillStyle = '#e1e8dc'; c.fillText(loc.name, p.x, p.y - 2);
      }
      if (this.showAxes) for (const axis of this.primaryAxes) {
        if (!axis.label) continue;
        const p = this.project(axis.label.x, axis.label.y);
        const offsetX = axis.name === 'N' ? -23 : axis.name === 'F' ? 15 : -3;
        const offsetY = axis.name === 'T' ? 16 : -12;
        c.font = 'bold 11px ui-monospace, monospace';
        const w = c.measureText(axis.name).width + 10;
        c.fillStyle = '#19323de8'; c.fillRect(p.x + offsetX - 5, p.y + offsetY - 11, w, 16);
        c.fillStyle = '#a7d5e8'; c.fillText(axis.name, p.x + offsetX, p.y + offsetY);
      }
    }
    this.dirty = false;
  }

  draw() {
    if (this.dirty) this.drawBase();
    const c = this.ctx;
    c.setTransform(1, 0, 0, 1, 0, 0); c.drawImage(this.base, 0, 0);
    c.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
    const selected = this.aircraft.find(a => a.id === this.selectedId);
    const controllerRoute = selected?.assignedRoute?.assignment?.source === 'controller' || selected?.route.assignment?.source === 'controller';
    if (controllerRoute && selected?.standardRoute) {
      this.path(c, selected.standardRoute.nodeIds.map(id => this.graph.nodes.get(id)!).filter(Boolean));
      c.setLineDash([5, 5]); c.strokeStyle = '#a3c86b66'; c.lineWidth = 1.5; c.stroke(); c.setLineDash([]);
    }
    if (selected?.assignedRoute && selected.conformance && selected.conformance.status !== 'on-route') {
      const points = selected.assignedRoute.nodeIds.map(id => this.graph.nodes.get(id)!).filter(Boolean);
      this.path(c, points); c.setLineDash([7, 5]); c.strokeStyle = '#81bdd5'; c.lineWidth = 2; c.stroke(); c.setLineDash([]);
    }
    const route = selected?.conformance?.status === 'unresolved' ? null : selected?.route || this.route;
    if (route) {
      const routeColor = controllerRoute || route.assignment?.source === 'controller' ? '#76c8ff' : '#c4ee7b';
      const points = route.nodeIds.map(id => this.graph.nodes.get(id)!).filter(Boolean);
      this.path(c, points); c.strokeStyle = `${routeColor}30`; c.lineWidth = 10; c.stroke();
      c.strokeStyle = routeColor; c.lineWidth = 2.5; c.stroke();
      for (const [index, text] of [[0, 'A'], [points.length - 1, 'B']] as const) {
        const point = points[index]; if (!point) continue;
        const p = this.project(point.x, point.y);
        c.beginPath(); c.arc(p.x, p.y, 10, 0, Math.PI * 2); c.fillStyle = routeColor; c.fill();
        c.font = 'bold 10px system-ui'; c.fillStyle = '#182329'; c.textAlign = 'center'; c.fillText(text, p.x, p.y + 3); c.textAlign = 'left';
      }
      for (const checkpoint of route.operational?.clearanceCheckpoints || []) {
        const node = this.graph.nodes.get(checkpoint.nodeId); if (!node) continue;
        const p = this.project(node.x, node.y);
        c.beginPath(); c.arc(p.x, p.y, 7, 0, Math.PI * 2); c.fillStyle = '#17242c'; c.fill();
        c.strokeStyle = '#f3b577'; c.lineWidth = 2; c.stroke();
        if (this.scale > .3) { c.fillStyle = '#f3cda0'; c.font = '9px ui-monospace, monospace'; c.fillText('ATTENTE / ATC', p.x + 11, p.y + 3); }
      }
      for (const handle of this.getRouteEditHandles()) {
        c.beginPath(); c.arc(handle.screenX, handle.screenY, 4.5, 0, Math.PI * 2);
        c.fillStyle = '#14232c'; c.fill(); c.strokeStyle = routeColor; c.lineWidth = 1.5; c.stroke();
      }
    }
    if (this.routeDragging) {
      const drag = this.routeDragging, source = this.project(drag.handle.x, drag.handle.y);
      const target = drag.targetNodeId ? this.graph.nodes.get(drag.targetNodeId)! : drag;
      const destination = this.project(target.x, target.y);
      c.beginPath(); c.moveTo(source.x, source.y); c.lineTo(destination.x, destination.y); c.setLineDash([5, 4]); c.strokeStyle = '#76c8ff'; c.lineWidth = 2; c.stroke(); c.setLineDash([]);
      c.beginPath(); c.arc(destination.x, destination.y, 9, 0, Math.PI * 2); c.strokeStyle = drag.targetNodeId ? '#76c8ff' : '#f3b577'; c.stroke();
      c.font = '10px system-ui'; c.fillStyle = '#d8eaf4'; c.fillText(drag.targetNodeId ? 'Attribuer par ce carrefour' : 'Rapprocher d’un carrefour', destination.x + 13, destination.y - 12);
    }
    if (this.showPrediction && selected && selected.state === 'taxiing') {
      // Sample only the actual planned polyline, including its turns.
      const from = selected.distance, to = Math.min(selected.route.distance, from + selected.speed * 60);
      const points: {x:number;y:number}[] = [{x:selected.x,y:selected.y}];
      selected.route.nodeIds.forEach((id, i) => {
        if (selected.route.cumulativeDistances[i] > from && selected.route.cumulativeDistances[i] <= to) points.push(this.graph.nodes.get(id)!);
      });
      const index = selected.route.cumulativeDistances.findIndex((d, i) => i > 0 && d >= to);
      if (index > 0) {
        const a = this.graph.nodes.get(selected.route.nodeIds[index - 1])!, b = this.graph.nodes.get(selected.route.nodeIds[index])!;
        const start = selected.route.cumulativeDistances[index - 1], length = selected.route.cumulativeDistances[index] - start;
        const fraction = length > 0 ? (to - start) / length : 0;
        points.push({ x: a.x + (b.x - a.x) * fraction, y: a.y + (b.y - a.y) * fraction });
      }
      this.path(c, points); c.setLineDash([5, 5]); c.strokeStyle = '#f1f6d5'; c.lineWidth = 3; c.stroke(); c.setLineDash([]);
    }
    const dangerous = new Set(this.conflicts.flatMap(f => f.aircraftIds));
    const activeAircraftCount = this.aircraft.filter(a => a.state !== 'scheduled').length;
    for (const a of this.aircraft) {
      if (a.state === 'scheduled') continue;
      const p = this.project(a.x, a.y);
      if (p.x < -20 || p.y < -20 || p.x > this.width + 20 || p.y > this.height + 20) continue;
      const offRoute = !!a.conformance && a.conformance.status !== 'on-route';
      const color = offRoute || dangerous.has(a.id) ? '#ff987b' : a.id === this.selectedId ? '#d7ff9b' : a.state === 'holding' ? '#e9c785' : a.state === 'arrived' ? '#7d9798' : '#93d8e0';
      if (this.showFootprints && (this.scale >= .3 || a.id === this.selectedId)) for (const polygon of footprintPolygons(a)) {
        this.path(c, polygon, true); c.fillStyle = `${color}10`; c.fill(); c.strokeStyle = `${color}65`; c.lineWidth = 1; c.stroke();
      }
      if (a.id === this.selectedId) { c.beginPath(); c.arc(p.x, p.y, 13, 0, Math.PI * 2); c.strokeStyle = '#c4ee7b80'; c.lineWidth = 1; c.stroke(); }
      c.save(); c.translate(p.x, p.y); c.rotate(-a.heading);
      c.beginPath(); c.moveTo(8, 0); c.lineTo(-5, -4); c.lineTo(-3, 0); c.lineTo(-5, 4); c.closePath(); c.fillStyle = color; c.fill(); c.restore();
      if (activeAircraftCount <= 25 || this.scale > .3 || a.id === this.selectedId) {
        c.font = `${a.id === this.selectedId ? 'bold ' : ''}10px ui-monospace, monospace`;
        c.fillStyle = '#132026dd'; c.fillRect(p.x + 9, p.y - 14, c.measureText(a.callsign).width + 6, 14);
        c.fillStyle = color; c.fillText(a.callsign, p.x + 12, p.y - 3);
        if (offRoute) { c.fillStyle = '#ff987b'; c.fillText(a.conformance?.status === 'unresolved' ? 'HORS ROUTE' : 'RACCORDEMENT', p.x + 12, p.y + 10); }
      }
    }
    for (const f of this.conflicts.slice(0, 50)) {
      const p = this.project(f.x, f.y);
      c.beginPath(); c.arc(p.x, p.y, f.severity === 'critical' ? 17 : 11, 0, Math.PI * 2);
      c.strokeStyle = f.severity === 'critical' ? '#ff886ba0' : '#eac98780'; c.lineWidth = 1; c.stroke();
    }
    // Scale bar is geographic, independent of CSS and device pixel ratio.
    const target = 110 / this.scale;
    const metres = [50, 100, 250, 500, 1000, 2000].reduce((a, b) => Math.abs(b - target) < Math.abs(a - target) ? b : a);
    const y = this.height - 30, x = 28, w = metres * this.scale;
    c.strokeStyle = '#b6c2c1'; c.lineWidth = 1; c.beginPath(); c.moveTo(x, y - 4); c.lineTo(x, y); c.lineTo(x + w, y); c.lineTo(x + w, y - 4); c.stroke();
    c.font = '10px ui-monospace, monospace'; c.fillStyle = '#b6c2c1'; c.fillText(metres >= 1000 ? `${metres / 1000} km` : `${metres} m`, x, y - 9);
  }
}
