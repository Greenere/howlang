import json

from collections import deque

from automata import _build_automata
from automata import _TERM_LITERAL, _TERM_OPERATOR, _TERM_RESERVED, _TERM_SPACE
from automata import _TERM_SEPARATOR, _TERM_VARIABLE, _TERM_BRACKET

term_type_map = {
    _TERM_RESERVED: "RESERVED",
    _TERM_SEPARATOR: "SEPARATOR",
    _TERM_VARIABLE: "VARIABLE",
    _TERM_OPERATOR: "OPERATOR",
    _TERM_SPACE: "SPACE",
    _TERM_BRACKET: "BRACKET",
    _TERM_LITERAL: "LITERAL"
}

def bind_states(start):
    visited = set()
    count = 0
    bind = {}
    queue = deque([start])
    while queue:
        n = len(queue)
        for _ in range(n):
            cur = queue.popleft()
            if cur not in visited:
                bind[count] = cur
                count += 1
            visited.add(cur)
            for nex in cur.branch.values():
                if nex not in visited:
                    queue.append(nex)
    return bind

def build_relation_graph():
    start = _build_automata()
    bind = bind_states(start)
    states = {v:k for k,v in bind.items()}
    graph = {}
    terms = {0:"START"}
    for k, v in bind.items():
        graph[k] = []
        for cond, nex in v.branch.items():
            graph[k].append((cond, states[nex]))
        if v.terminal:
            terms[k] = term_type_map[v.terminal_type]
    
    for key, nei in graph.items():
        jump_set = {}
        for jump in nei:
            if jump[1] not in jump_set:
                jump_set[jump[1]] = jump[0]
            else:
                jump_set[jump[1]] += '/' + jump[0]
        graph[key] = [(v, k) for k, v in jump_set.items()]

    edges = []
    for k, nei in graph.items():
        for n in nei:
            edges.append([k, n[1], n[0]])
    
    links = []
    for s,t, cond in edges:
        links.append({
            'source':str(s),
            'target':str(t),
            'condition':cond
        })
    
    nodes = []
    for key in graph.keys():
        nodes.append({
            "id":str(key),
            "type": "NORMAL" if key not in terms else terms[key]
        })

    return graph, terms, edges, nodes, links

if __name__ == "__main__":
    graph, terms, edges, nodes, links = build_relation_graph()
    automata = {
        'graph':graph,
        'terms': terms,
        'start': 0,
        'edges':edges,
        'data':{
            'links':links,
            'nodes':nodes
        }
    }
    with open('automata.json', 'w') as f:
        json.dump(automata, f, indent = 2)
