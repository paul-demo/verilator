// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Graph acyclic algorithm
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#define VL_MT_DISABLED_CODE_UNIT 1

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3Graph.h"

#include <algorithm>
#include <list>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
//######################################################################
// Algorithms - acyclic
//      Break the minimal number of backward edges to make the graph acyclic

class GraphAcycVertex final : public V3GraphVertex {
    VL_RTTI_IMPL(GraphAcycVertex, V3GraphVertex)
    // user() is used for various sub-algorithm pieces
    V3GraphVertex* const m_origVertexp;  // Pointer to first vertex this represents
protected:
    friend class GraphAcyc;
    V3ListLinks<GraphAcycVertex> m_links;  // List links to store instances of this class
    uint32_t m_storedRank = 0;  // Rank held until commit to edge placement
    bool m_onWorkList = false;  // True if already on list of work to do
    bool m_deleted = false;  // True if deleted

private:
    V3ListLinks<GraphAcycVertex>& links() { return m_links; }

public:
    // List type to store instances of this class
    using List = V3List<GraphAcycVertex, &GraphAcycVertex::links>;

    GraphAcycVertex(V3Graph* graphp, V3GraphVertex* origVertexp)
        : V3GraphVertex{graphp}
        , m_origVertexp{origVertexp} {}
    ~GraphAcycVertex() override = default;
    V3GraphVertex* origVertexp() const { return m_origVertexp; }
    void setDelete() { m_deleted = true; }
    bool isDelete() const { return m_deleted; }
    string name() const override { return m_origVertexp->name(); }
    string dotColor() const override { return m_origVertexp->dotColor(); }
    FileLine* fileline() const override { return m_origVertexp->fileline(); }
};

//--------------------------------------------------------------------

class GraphAcycEdge final : public V3GraphEdge {
    VL_RTTI_IMPL(GraphAcycEdge, V3GraphEdge)
    // userp() is always used to point to the head original graph edge
private:
    using OrigEdgeList = std::list<V3GraphEdge*>;  // List of orig edges, see also GraphAcyc's decl
    V3GraphEdge* origEdgep() const {
        const OrigEdgeList* const oEListp = static_cast<OrigEdgeList*>(userp());
        UASSERT(oEListp, "No original edge associated with acyc edge " << this);
        return (oEListp->front());
    }

public:
    GraphAcycEdge(V3Graph* graphp, V3GraphVertex* fromp, V3GraphVertex* top, int weight,
                  bool cutable = false)
        : V3GraphEdge{graphp, fromp, top, weight, cutable} {}
    ~GraphAcycEdge() override = default;
    // yellow=we might still cut it, else oldEdge: yellowGreen=made uncutable, red=uncutable
    string dotColor() const override { return (cutable() ? "yellow" : origEdgep()->dotColor()); }
};

//--------------------------------------------------------------------

struct GraphAcycEdgeCmp final {
    bool operator()(const V3GraphEdge* lhsp, const V3GraphEdge* rhsp) const {
        if (lhsp->weight() > rhsp->weight()) return true;  // LHS goes first
        if (lhsp->weight() < rhsp->weight()) return false;  // RHS goes first
        return false;
    }
};

//--------------------------------------------------------------------

// CLASSES
class GraphAcyc final {
    using OrigEdgeList
        = std::list<V3GraphEdge*>;  // List of orig edges, see also GraphAcycEdge's decl
    // GRAPH USERS
    //  origGraph
    //    GraphVertex::user()   GraphAycVerted* New graph node
    //  m_breakGraph
    //    GraphEdge::user()     OrigEdgeList*   Old graph edges
    //    GraphVertex::user     bool            Detection of loops in simplifyDupIterate
    // MEMBERS
    V3Graph* const m_origGraphp;  // Original graph
    V3Graph m_breakGraph;  // Graph with only breakable edges represented
    GraphAcycVertex::List m_work;  // List of vertices with optimization work left
    std::vector<OrigEdgeList*> m_origEdgeDelp;  // List of deletions to do when done
    const V3EdgeFuncP
        m_origEdgeFuncp;  // Function that says we follow this edge (in original graph)
    uint32_t m_placeStep = 0;  // Number that user() must be equal to to indicate processing

    // METHODS
    void buildGraph(V3Graph* origGraphp);
    void buildGraphIterate(V3GraphVertex* overtexp, GraphAcycVertex* avertexp);
    void simplify(bool allowCut);
    void simplifyNone(GraphAcycVertex* avertexp);
    void simplifyOne(GraphAcycVertex* avertexp);
    void simplifyOut(GraphAcycVertex* avertexp);
    void simplifyDup(GraphAcycVertex* avertexp);
    void cutBasic(GraphAcycVertex* avertexp);
    void cutBackward(GraphAcycVertex* avertexp);
    void deleteMarked();
    void place();
    void placeTryEdge(V3GraphEdge* edgep);
    bool placeIterate(GraphAcycVertex* vertexp, uint32_t currentRank);

    bool origFollowEdge(V3GraphEdge* edgep) {
        return (edgep->weight() && (m_origEdgeFuncp)(edgep));
    }
    void edgeFromEdge(V3GraphEdge* oldedgep, V3GraphVertex* fromp, V3GraphVertex* top) {
        // Make new breakGraph edge, with old edge as a template
        GraphAcycEdge* const newEdgep = new GraphAcycEdge{&m_breakGraph, fromp, top,
                                                          oldedgep->weight(), oldedgep->cutable()};
        newEdgep->userp(oldedgep->userp());  // Keep pointer to OrigEdgeList
    }
    void addOrigEdgep(V3GraphEdge* toEdgep, V3GraphEdge* addEdgep) {
        // Add addEdge (or it's list) to list of edges that break edge represents
        // Note addEdge may already have a bunch of similar linked edge representations.  Yuk.
        UASSERT(addEdgep, "Adding nullptr");
        if (!toEdgep->userp()) {
            OrigEdgeList* const oep = new OrigEdgeList;
            m_origEdgeDelp.push_back(oep);
            toEdgep->userp(oep);
        }
        OrigEdgeList* const oEListp = static_cast<OrigEdgeList*>(toEdgep->userp());
        if (OrigEdgeList* const addListp = static_cast<OrigEdgeList*>(addEdgep->userp())) {
            for (const auto& itr : *addListp) oEListp->push_back(itr);
            addListp->clear();  // Done with it
        } else {
            oEListp->push_back(addEdgep);
        }
    }
    void cutOrigEdge(V3GraphEdge* breakEdgep, const char* why) {
        // From the break edge, cut edges in original graph it represents
        UINFO(8, why << " CUT " << breakEdgep->fromp());
        breakEdgep->cut();
        const OrigEdgeList* const oEListp = static_cast<OrigEdgeList*>(breakEdgep->userp());
        if (!oEListp) {
            v3fatalSrc("No original edge associated with cutting edge " << breakEdgep);
        }
        // The breakGraph edge may represent multiple real edges; cut them all
        for (const auto& origEdgep : *oEListp) {
            origEdgep->cut();
            UINFO(8, "  " << why << "   " << origEdgep->fromp() << " ->" << origEdgep->top());
        }
    }
    // Work Queue
    void workPush(V3GraphVertex* vertexp) {
        GraphAcycVertex* const avertexp = static_cast<GraphAcycVertex*>(vertexp);
        // Add vertex to list of nodes needing further optimization trials
        if (!avertexp->m_onWorkList) {
            avertexp->m_onWorkList = true;
            m_work.linkBack(avertexp);
        }
    }
    GraphAcycVertex* workBeginp() { return m_work.frontp(); }
    void workPop() {
        GraphAcycVertex* const avertexp = workBeginp();
        avertexp->m_onWorkList = false;
        m_work.unlink(avertexp);
    }

public:
    // CONSTRUCTORS
    GraphAcyc(V3Graph* origGraphp, V3EdgeFuncP edgeFuncp)
        : m_origGraphp{origGraphp}
        , m_origEdgeFuncp{edgeFuncp} {}
    ~GraphAcyc() {
        for (OrigEdgeList* ip : m_origEdgeDelp) delete ip;
        m_origEdgeDelp.clear();
    }
    void main();
};

//--------------------------------------------------------------------

void GraphAcyc::buildGraph(V3Graph* origGraphp) {
    // Presumes the graph has been strongly ordered,
    // and thus there's a unique color if there are loops in this subgraph.

    // For each old node, make a new graph node for optimization
    origGraphp->userClearVertices();
    origGraphp->userClearEdges();
    for (V3GraphVertex& overtex : origGraphp->vertices()) {
        if (overtex.color()) {
            GraphAcycVertex* const avertexp = new GraphAcycVertex{&m_breakGraph, &overtex};
            overtex.userp(avertexp);  // Stash so can look up later
        }
    }

    // Build edges between logic vertices
    for (V3GraphVertex& overtex : origGraphp->vertices()) {
        if (overtex.color()) {
            GraphAcycVertex* const avertexp = static_cast<GraphAcycVertex*>(overtex.userp());
            buildGraphIterate(&overtex, avertexp);
        }
    }
}

void GraphAcyc::buildGraphIterate(V3GraphVertex* overtexp, GraphAcycVertex* avertexp) {
    // Make new edges
    for (V3GraphEdge& edge : overtexp->outEdges()) {
        if (origFollowEdge(&edge)) {  // not cut
            const V3GraphVertex* toVertexp = edge.top();
            if (toVertexp->color()) {
                GraphAcycVertex* const toAVertexp
                    = static_cast<GraphAcycVertex*>(toVertexp->userp());
                // Replicate the old edge into the new graph
                // There may be multiple edges between same pairs of vertices
                V3GraphEdge* breakEdgep = new GraphAcycEdge{&m_breakGraph, avertexp, toAVertexp,
                                                            edge.weight(), edge.cutable()};
                addOrigEdgep(breakEdgep, &edge);  // So can find original edge
            }
        }
    }
}

void GraphAcyc::simplify(bool allowCut) {
    // Add all nodes to list of work to do
    for (V3GraphVertex& vertex : m_breakGraph.vertices()) workPush(&vertex);
    // Optimize till everything finished
    while (GraphAcycVertex* vertexp = workBeginp()) {
        workPop();
        simplifyNone(vertexp);
        simplifyOne(vertexp);
        simplifyOut(vertexp);
        simplifyDup(vertexp);
        if (allowCut) {
            // The main algorithm works without these, though slower
            // So if changing the main algorithm, comment these out for a test run
            if (v3Global.opt.fAcycSimp()) {
                cutBasic(vertexp);
                cutBackward(vertexp);
            }
        }
    }
    deleteMarked();
}

void GraphAcyc::deleteMarked() {
    // Delete nodes marked for removal
    for (V3GraphVertex* const vtxp : m_breakGraph.vertices().unlinkable()) {
        GraphAcycVertex* const avertexp = static_cast<GraphAcycVertex*>(vtxp);
        if (avertexp->isDelete()) {
            VL_DO_DANGLING(avertexp->unlinkDelete(&m_breakGraph), avertexp);
        }
    }
}

void GraphAcyc::simplifyNone(GraphAcycVertex* avertexp) {
    // Don't need any vertices with no inputs, There's no way they can have a loop.
    // Likewise, vertices with no outputs
    if (avertexp->isDelete()) return;
    if (avertexp->inEmpty() || avertexp->outEmpty()) {
        UINFO(9, "  SimplifyNoneRemove " << avertexp);
        avertexp->setDelete();  // Mark so we won't delete it twice
        // Remove edges
        while (V3GraphEdge* const edgep = avertexp->outEdges().frontp()) {
            workPush(edgep->top());
            VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
        }
        while (V3GraphEdge* const edgep = avertexp->inEdges().frontp()) {
            workPush(edgep->fromp());
            VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
        }
    }
}

void GraphAcyc::simplifyOne(GraphAcycVertex* avertexp) {
    // If a node has one input and one output, we can remove it and change the edges
    if (avertexp->isDelete()) return;
    if (avertexp->inSize1() && avertexp->outSize1()) {
        V3GraphEdge* const inEdgep = avertexp->inEdges().frontp();
        V3GraphEdge* const outEdgep = avertexp->outEdges().frontp();
        V3GraphVertex* const inVertexp = inEdgep->fromp();
        V3GraphVertex* const outVertexp = outEdgep->top();
        // The in and out may be the same node; we'll make a loop
        // The in OR out may be THIS node; we can't delete it then.
        if (inVertexp != avertexp && outVertexp != avertexp) {
            UINFO(9, "  SimplifyOneRemove " << avertexp);
            avertexp->setDelete();  // Mark so we won't delete it twice
            // Make a new edge connecting the two vertices directly
            // If both are breakable, we pick the one with less weight, else it's arbitrary
            // We can forget about the origEdge list for the "non-selected" set of edges,
            // as we need to break only one set or the other set of edges, not both.
            // (This is why we must give preference to the cutable set.)
            V3GraphEdge* const templateEdgep
                = ((inEdgep->cutable()
                    && (!outEdgep->cutable() || inEdgep->weight() < outEdgep->weight()))
                       ? inEdgep
                       : outEdgep);
            // cppcheck-suppress leakReturnValNotUsed
            edgeFromEdge(templateEdgep, inVertexp, outVertexp);
            // Remove old edge
            VL_DO_DANGLING(inEdgep->unlinkDelete(), inEdgep);
            VL_DO_DANGLING(outEdgep->unlinkDelete(), outEdgep);
            VL_DANGLING(templateEdgep);
            workPush(inVertexp);
            workPush(outVertexp);
        }
    }
}

void GraphAcyc::simplifyOut(GraphAcycVertex* avertexp) {
    // If a node has one output that's not cutable, all its inputs can be reassigned
    // to the next node in the list
    if (avertexp->isDelete()) return;
    if (avertexp->outSize1()) {
        V3GraphEdge* const outEdgep = avertexp->outEdges().frontp();
        if (!outEdgep->cutable()) {
            V3GraphVertex* outVertexp = outEdgep->top();
            UINFO(9, "  SimplifyOutRemove " << avertexp);
            avertexp->setDelete();  // Mark so we won't delete it twice
            for (V3GraphEdge* const inEdgep : avertexp->inEdges().unlinkable()) {
                V3GraphVertex* inVertexp = inEdgep->fromp();
                if (inVertexp == avertexp) {
                    if (debug()) v3error("Non-cutable vertex=" << avertexp);  // LCOV_EXCL_LINE
                    v3error("Circular logic when ordering code (non-cutable edge loop)\n"
                            << m_origGraphp->reportLoops(  // calls OrderGraph::loopsVertexCb
                                   &V3GraphEdge::followNotCutable, avertexp->origVertexp()));
                    // Things are unlikely to end well at this point,
                    // but we'll try something to get to further errors...
                    inEdgep->cutable(true);
                    return;
                }
                // Make a new edge connecting the two vertices directly
                // cppcheck-suppress leakReturnValNotUsed
                edgeFromEdge(inEdgep, inVertexp, outVertexp);
                // Remove old edge
                VL_DO_DANGLING(inEdgep->unlinkDelete(), inEdgep);
                workPush(inVertexp);
            }
            VL_DO_DANGLING(outEdgep->unlinkDelete(), outEdgep);
            workPush(outVertexp);
        }
    }
}

void GraphAcyc::simplifyDup(GraphAcycVertex* avertexp) {
    // Remove redundant edges
    if (avertexp->isDelete()) return;
    // Clear marks
    for (V3GraphEdge& edge : avertexp->outEdges()) edge.top()->userp(nullptr);
    // Mark edges and detect duplications
    for (V3GraphEdge* const edgep : avertexp->outEdges().unlinkable()) {
        V3GraphVertex* outVertexp = edgep->top();
        V3GraphEdge* prevEdgep = static_cast<V3GraphEdge*>(outVertexp->userp());
        if (prevEdgep) {
            if (!prevEdgep->cutable()) {
                // !cutable duplicates prev !cutable: we can ignore it, redundant
                //  cutable duplicates prev !cutable: know it's not a relevant loop, ignore it
                UINFO(8, "    DelDupEdge " << avertexp << " -> " << edgep->top());
                VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
            } else if (!edgep->cutable()) {
                // !cutable duplicates prev  cutable: delete the earlier cutable
                UINFO(8, "    DelDupPrev " << avertexp << " -> " << prevEdgep->top());
                VL_DO_DANGLING(prevEdgep->unlinkDelete(), prevEdgep);
                outVertexp->userp(edgep);
            } else {
                //  cutable duplicates prev  cutable: combine weights
                UINFO(8, "    DelDupComb " << avertexp << " -> " << edgep->top());
                prevEdgep->weight(prevEdgep->weight() + edgep->weight());
                addOrigEdgep(prevEdgep, edgep);
                VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
            }
            workPush(outVertexp);
            workPush(avertexp);
        } else {
            // No previous assignment
            outVertexp->userp(edgep);
        }
    }
}

void GraphAcyc::cutBasic(GraphAcycVertex* avertexp) {
    // Detect and cleanup any loops from node to itself
    if (avertexp->isDelete()) return;
    for (V3GraphEdge* const edgep : avertexp->outEdges().unlinkable()) {
        if (edgep->cutable() && edgep->top() == avertexp) {
            cutOrigEdge(edgep, "  Cut Basic");
            VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
            workPush(avertexp);
        }
    }
}

void GraphAcyc::cutBackward(GraphAcycVertex* avertexp) {
    // If a cutable edge is from A->B, and there's a non-cutable edge B->A, then must cut!
    if (avertexp->isDelete()) return;
    // Clear marks
    for (V3GraphEdge& edge : avertexp->outEdges()) edge.top()->user(false);
    for (V3GraphEdge& edge : avertexp->inEdges()) {
        if (!edge.cutable()) edge.fromp()->user(true);
    }
    // Detect duplications
    for (V3GraphEdge* const edgep : avertexp->outEdges().unlinkable()) {
        if (edgep->cutable() && edgep->top()->user()) {
            cutOrigEdge(edgep, "  Cut A->B->A");
            VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
            workPush(avertexp);
        }
    }
}

void GraphAcyc::place() {
    // Input is m_breakGraph with ranks already assigned on non-breakable edges

    // Make a list of all cutable edges in the graph
    int numEdges = 0;
    for (V3GraphVertex& vertex : m_breakGraph.vertices()) {
        for (V3GraphEdge& edge : vertex.outEdges()) {
            if (edge.weight() && edge.cutable()) ++numEdges;
        }
    }
    UINFO(4, "    Cutable edges = " << numEdges);

    std::vector<V3GraphEdge*> edges;  // List of all edges to be processed
    // Make the vector properly sized right off the bat -- faster than reallocating
    edges.reserve(numEdges + 1);
    for (V3GraphVertex& vertex : m_breakGraph.vertices()) {
        vertex.user(0);  // Clear in prep of next step
        for (V3GraphEdge& edge : vertex.outEdges()) {
            if (edge.weight() && edge.cutable()) edges.push_back(&edge);
        }
    }

    // Sort by weight, then by vertex (so that we completely process one vertex, when possible)
    stable_sort(edges.begin(), edges.end(), GraphAcycEdgeCmp());

    // Process each edge in weighted order
    m_placeStep = 10;
    for (V3GraphEdge* edgep : edges) placeTryEdge(edgep);
}

void GraphAcyc::placeTryEdge(V3GraphEdge* edgep) {
    // Try to make this edge uncutable
    m_placeStep++;
    UINFO(8, "    PlaceEdge s" << m_placeStep << " w" << edgep->weight() << " " << edgep->fromp());
    // Make the edge uncutable so we detect it in placement
    edgep->cutable(false);
    // Vertex::m_user begin: number indicates this edge was completed
    // Try to assign ranks, presuming this edge is in place
    // If we come across user()==placestep, we've detected a loop and must back out
    const bool loop
        = placeIterate(static_cast<GraphAcycVertex*>(edgep->top()), edgep->fromp()->rank() + 1);
    if (!loop) {
        // No loop, we can keep it as uncutable
        // Commit the new ranks we calculated
        // Just cleanup the list.  If this is slow, we can add another set of
        // user counters to avoid cleaning up the list.
        while (workBeginp()) workPop();
    } else {
        // Adding this edge would cause a loop, kill it
        edgep->cutable(true);  // So graph still looks pretty
        cutOrigEdge(edgep, "  Cut loop");
        VL_DO_DANGLING(edgep->unlinkDelete(), edgep);
        // Back out the ranks we calculated
        while (GraphAcycVertex* vertexp = workBeginp()) {
            workPop();
            vertexp->rank(vertexp->m_storedRank);
        }
    }
}

bool GraphAcyc::placeIterate(GraphAcycVertex* vertexp, uint32_t currentRank) {
    // Assign rank to each unvisited node
    //   rank() is the "committed rank" of the graph known without loops
    // If larger rank is found, assign it and loop back through
    // If we hit a back node make a list of all loops
    if (vertexp->rank() >= currentRank) return false;  // Already processed it
    if (vertexp->user() == m_placeStep) return true;  // Loop detected
    vertexp->user(m_placeStep);
    // Remember we're changing the rank of this node; might need to back out
    if (!vertexp->m_onWorkList) {
        vertexp->m_storedRank = vertexp->rank();
        workPush(vertexp);
    }
    vertexp->rank(currentRank);
    // Follow all edges and increase their ranks
    for (V3GraphEdge& edge : vertexp->outEdges()) {
        if (edge.weight() && !edge.cutable()) {
            if (placeIterate(static_cast<GraphAcycVertex*>(edge.top()), currentRank + 1)) {
                // We don't need to reset user(); we'll use a different placeStep for the next edge
                return true;  // Loop detected
            }
        }
    }
    vertexp->user(0);
    return false;
}

//----- Main algorithm entry point

void GraphAcyc::main() {
    m_breakGraph.userClearEdges();

    // Color based on possible loops
    m_origGraphp->stronglyConnected(m_origEdgeFuncp);

    // Make a new graph with vertices that have only a single vertex
    // for each group of old vertices that are interconnected with unbreakable
    // edges (and thus can't represent loops - if we did the unbreakable
    // marking right, anyways)
    buildGraph(m_origGraphp);
    if (dumpGraphLevel() >= 6) m_breakGraph.dumpDotFilePrefixed("acyc_pre");

    // Perform simple optimizations before any cuttings
    simplify(false);
    if (dumpGraphLevel() >= 5) m_breakGraph.dumpDotFilePrefixed("acyc_simp");

    UINFO(4, " Cutting trivial loops");
    simplify(true);
    if (dumpGraphLevel() >= 6) m_breakGraph.dumpDotFilePrefixed("acyc_mid");

    UINFO(4, " Ranking");
    m_breakGraph.rank(&V3GraphEdge::followNotCutable);
    if (dumpGraphLevel() >= 6) m_breakGraph.dumpDotFilePrefixed("acyc_rank");

    UINFO(4, " Placement");
    place();
    if (dumpGraphLevel() >= 6) m_breakGraph.dumpDotFilePrefixed("acyc_place");

    UINFO(4, " Final Ranking");
    // Only needed to assert there are no loops in completed graph
    m_breakGraph.rank(&V3GraphEdge::followAlwaysTrue);
    if (dumpGraphLevel() >= 6) m_breakGraph.dumpDotFilePrefixed("acyc_done");
}

void V3Graph::acyclic(V3EdgeFuncP edgeFuncp) {
    UINFO(4, "Acyclic");
    GraphAcyc acyc{this, edgeFuncp};
    acyc.main();
    UINFO(4, "Acyclic done");
}
