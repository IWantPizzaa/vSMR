import type { AirportData, AirportLocation, GraphArc, TaxiEdge, TaxiNode } from './types';

export class TaxiGraph {
  readonly nodes = new Map<string, TaxiNode>();
  readonly edges = new Map<string, TaxiEdge>();
  readonly adjacency = new Map<string, GraphArc[]>();
  readonly components: string[][] = [];
  readonly componentByNode = new Map<string, number>();
  readonly locations: AirportLocation[];

  constructor(readonly data: AirportData) {
    this.locations = data.locations;
    for (const node of data.nodes) {
      if (this.nodes.has(node.id) || !Number.isFinite(node.x) || !Number.isFinite(node.y)) throw new Error(`Invalid node ${node.id}`);
      this.nodes.set(node.id, node);
      this.adjacency.set(node.id, []);
    }
    for (const edge of data.edges) {
      if (this.edges.has(edge.id) || !this.nodes.has(edge.from) || !this.nodes.has(edge.to) || !Number.isFinite(edge.length) || edge.length <= 0) throw new Error(`Invalid edge ${edge.id}`);
      const geometric = this.distance(edge.from, edge.to);
      if (edge.length + 0.02 < geometric) throw new Error(`Edge ${edge.id} is shorter than its geometry`);
      this.edges.set(edge.id, edge);
      this.adjacency.get(edge.from)!.push({ to: edge.to, edgeId: edge.id, length: edge.length });
      this.adjacency.get(edge.to)!.push({ to: edge.from, edgeId: edge.id, length: edge.length });
    }
    for (const start of this.nodes.keys()) {
      if (this.componentByNode.has(start)) continue;
      const index = this.components.length, component: string[] = [start];
      this.componentByNode.set(start, index);
      for (let i = 0; i < component.length; i++) {
        for (const arc of this.adjacency.get(component[i])!) {
          if (!this.componentByNode.has(arc.to)) { this.componentByNode.set(arc.to, index); component.push(arc.to); }
        }
      }
      this.components.push(component);
    }
    this.components.sort((a, b) => b.length - a.length || a[0].localeCompare(b[0]));
    this.componentByNode.clear();
    this.components.forEach((component, index) => component.forEach(id => this.componentByNode.set(id, index)));
  }

  distance(a: string, b: string): number {
    const p = this.nodes.get(a), q = this.nodes.get(b);
    if (!p || !q) return Infinity;
    return Math.hypot(p.x - q.x, p.y - q.y);
  }

  nearestNode(x: number, y: number, maxDistance = Infinity): TaxiNode | null {
    let best: TaxiNode | null = null, distance = maxDistance;
    for (const node of this.nodes.values()) {
      if (!this.adjacency.get(node.id)?.length) continue;
      const d = Math.hypot(node.x - x, node.y - y);
      if (d < distance) { best = node; distance = d; }
    }
    return best;
  }

  areConnected(a: string, b: string): boolean {
    return this.componentByNode.has(a) && this.componentByNode.get(a) === this.componentByNode.get(b);
  }
}
