#!/usr/bin/env python3
"""
Check for circular dependencies in the dependency graph.
Usage: python3 tools/check_circular_deps.py deps_graph.json
"""

import json
import sys
from collections import defaultdict

def load_deps(filepath):
    with open(filepath, 'r') as f:
        return json.load(f)

def find_circular_deps(deps):
    """Find circular dependencies using DFS."""
    files = deps.get('files', {})
    
    # Build adjacency list
    graph = defaultdict(set)
    for file, info in files.items():
        for dep in info.get('depends_on', []):
            graph[file].add(dep)
    
    # DFS to find cycles
    visited = set()
    rec_stack = set()
    cycles = []
    
    def dfs(node, path):
        visited.add(node)
        rec_stack.add(node)
        path.append(node)
        
        for neighbor in graph.get(node, []):
            if neighbor not in visited:
                result = dfs(neighbor, path)
                if result:
                    return result
            elif neighbor in rec_stack:
                # Found a cycle
                cycle_start = path.index(neighbor)
                cycle = path[cycle_start:] + [neighbor]
                return cycle
        
        path.pop()
        rec_stack.remove(node)
        return None
    
    for node in graph:
        if node not in visited:
            cycle = dfs(node, [])
            if cycle:
                cycles.append(cycle)
    
    return cycles

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 check_circular_deps.py <deps_graph.json>")
        sys.exit(1)
    
    deps = load_deps(sys.argv[1])
    cycles = find_circular_deps(deps)
    
    if cycles:
        print(f"Found {len(cycles)} circular dependency cycle(s):")
        for i, cycle in enumerate(cycles, 1):
            print(f"\nCycle {i}:")
            for j, node in enumerate(cycle):
                print(f"  {j+1}. {node}")
        sys.exit(1)
    else:
        print("No circular dependencies found!")
        sys.exit(0)

if __name__ == '__main__':
    main()
