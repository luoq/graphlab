#ifndef FROM_DISTRIBUTED_GRAPH_INCLUDE
#warning "distributed_graph_synchronization.hpp should not be included directly."
#warning "You should include only distributed_graph.hpp"
#warning "I will fix this for you now, but don't do it again!"

#include <graphlab/distributed2/graph/distributed_graph.hpp>

#else



/**
 * In a synchronous way, synchronize the data on vertex with global id vid
 * vid must be a ghost
 */
template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_vertex(vertex_id_t vid, bool async) {
  vertex_id_t localvid = global2localvid[vid];
  if (is_ghost(vid)) {
    vertex_conditional_store out;
    out.hasdata = localstore.vertex_modified(localvid);
    if (out.hasdata) out.data.first = localstore.vertex_data(localvid);
    if (async == false) {
      vertex_conditional_store v;
      v = rmi.remote_request(localvid2owner[localvid],
                            &distributed_graph<VertexData, EdgeData>::get_vertex_if_version_less_than,
                            vid,
                            localstore.vertex_version(localvid),
                            out);
     if (v.hasdata) {
        localstore.vertex_data(localvid) = v.data.first;
        localstore.set_vertex_version(localvid, v.data.second);
      }
    }
    else {
      pending_async_updates.inc();
      rmi.remote_call(localvid2owner[localvid],
                       &distributed_graph<VertexData, EdgeData>::async_get_vertex_if_version_less_than,
                       rmi.procid(),
                       vid,
                       localstore.vertex_version(localvid),
                       out);
    }
  }
}

/**
 * In a synchronous way, synchronize the data on edge with global id eid
 * target of edge must be a ghost
 */
template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_edge(edge_id_t eid, bool async) {

  if (!edge_canonical_numbering) {
    edge_id_t localeid = global2localeid[eid];

    edge_conditional_store out;
    out.hasdata = localstore.edge_modified(localeid);
    if (out.hasdata) out.data.first = localstore.edge_data(localeid);

    if (localvid2owner[localstore.target(localeid)] != rmi.procid()) {
      if (async == false) {
        edge_conditional_store e = rmi.remote_request(localvid2owner[localstore.target(localeid)],
                                                    &distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than,
                                                    eid,
                                                    localstore.edge_version(localeid),
                                                    out);
        if (e.hasdata) {
          localstore.edge_data(localeid) = e.data.first;
          localstore.set_edge_version(localeid, e.data.second);
        }
      }
      else {
        pending_async_updates.inc();
        rmi.remote_call(localvid2owner[localstore.target(localeid)],
                       &distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than,
                       rmi.procid(),
                       eid,
                       localstore.edge_version(localeid),
                       out);
      }
    }
  }
  else {
    edge_id_t localeid = eid;
    vertex_id_t localtargetvid = localstore.target(localeid);
    vertex_id_t targetvid = local2globalvid[localtargetvid];
    vertex_id_t localsourcevid = localstore.source(localeid);
    vertex_id_t sourcevid = local2globalvid[localsourcevid];

    if (is_ghost(targetvid)) {

      edge_conditional_store out;
      out.hasdata = localstore.edge_modified(localeid);
      if (out.hasdata) out.data.first = localstore.edge_data(localeid);
      if (async == false) {
        edge_conditional_store e = rmi.remote_request(localvid2owner[localtargetvid],
                                                      &distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than2,
                                                      sourcevid,
                                                      targetvid,
                                                      localstore.edge_version(localeid),
                                                      out);
        if (e.hasdata) {
          localstore.edge_data(localeid) = e.data.first;
          localstore.set_edge_version(localeid, e.data.second);
        }
      }
      else {
        pending_async_updates.inc();
        rmi.remote_call(localvid2owner[localtargetvid],
                         &distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than2,
                         rmi.procid(),
                         sourcevid,
                         targetvid,
                         localstore.edge_version(localeid),
                         out);
      }
    }
  }
}





/**
 * In a synchronous way, synchronize the entire scope for vertex vid
 * vid must be owned by the current machine
 */
template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_scope(vertex_id_t vid, bool async) {
  ASSERT_FALSE(is_ghost(vid));
  // now this is very annoying. A significant amount of code is identical here.
  // whether with edge canonical numbering on or not. But I cannot refactor it
  // easily because the types are different and I don't really want to introduce
  // templates here for something so trivial.
  
  if (!edge_canonical_numbering) {
    vertex_id_t localvid = global2localvid[vid];
    std::map<procid_t, 
            std::pair<block_synchronize_request, std::vector<vertex_id_t>::iterator> > requests;
    // I should have all the in-edges. but I need the in vertices.
    // need to track the vertices added so I don't add duplicate vertices
    // if the vertex has both in-out edges to this vertex.
    // trick! vids are ordered!

    foreach(edge_id_t localineid, localstore.in_edge_ids(localvid)) {
      vertex_id_t localsourcevid = localstore.source(localineid);
      if (localvid_is_ghost(localsourcevid)) {
        // need to synchronize incoming vertex
        procid_t targetowner = localvid2owner[localsourcevid];
        block_synchronize_request &req = requests[targetowner].first;
        req.vid.push_back(local2globalvid[localsourcevid]);
        req.vidversion.push_back(localstore.vertex_version(localsourcevid));
        vertex_conditional_store vs;
        vs.hasdata = localstore.vertex_modified(localsourcevid);
        if (vs.hasdata) {
          localstore.set_vertex_modified(localsourcevid, false);
          vs.data.first = localstore.vertex_data(localsourcevid);
          vs.data.second = 0; // unused
        }
        requests[targetowner].second=req.vid.end();
        req.vstore.push_back(vs);
      }
    }
    // now for the out edges
    foreach(edge_id_t localouteid, localstore.out_edge_ids(localvid)) {
      vertex_id_t localtargetvid = localstore.target(localouteid);
      procid_t targetowner = localvid2owner[localstore.target(localouteid)];
      block_synchronize_request &req = requests[targetowner].first;

      if (localvid_is_ghost(localtargetvid)) {
        // need to synchronize outgoing vertex and outgoing edge
        // do outgoing vertex first
        if (std::binary_search(req.vid.begin(), requests[targetowner].second, local2globalvid[localtargetvid]) == false) {
          req.vid.push_back(local2globalvid[localtargetvid]);
          req.vidversion.push_back(localstore.vertex_version(localtargetvid));
          vertex_conditional_store vs;
          vs.hasdata = localstore.vertex_modified(localtargetvid);
          if (vs.hasdata) {
            localstore.set_vertex_modified(localtargetvid, false);
            vs.data.first = localstore.vertex_data(localtargetvid);
            vs.data.second = 0; // unused
          }
          req.vstore.push_back(vs);
        }
        // now for the outgoing edge
        
        req.eid.push_back(local2globaleid[localouteid]);
        req.edgeversion.push_back(localstore.edge_version(localouteid));
        edge_conditional_store es;
        es.hasdata = localstore.edge_modified(localouteid);
        if (es.hasdata) {
          localstore.set_edge_modified(localouteid, false);
          es.data.first = localstore.edge_data(localouteid);
          es.data.second = 0;
        }
        req.estore.push_back(es);
      }
    }
    
    if (async == false) {
      typename std::map<procid_t, 
            std::pair<block_synchronize_request, std::vector<vertex_id_t>::iterator> >::iterator iter;
      iter = requests.begin();
      while(iter != requests.end()) {
        iter->second.first = rmi.remote_request(iter->first,
                                          &distributed_graph<VertexData, EdgeData>::get_alot,
                                          iter->second.first);
        // unpack
        update_alot(iter->second.first);
        ++iter;
      }
    }
    else {
      typename std::map<procid_t, 
            std::pair<block_synchronize_request, std::vector<vertex_id_t>::iterator> >::iterator iter;
      iter = requests.begin();
  
      while(iter != requests.end()) {
        pending_async_updates.inc();
        rmi.remote_call(iter->first,
                         &distributed_graph<VertexData, EdgeData>::async_get_alot,
                         rmi.procid(),
                         iter->second.first);
        ++iter;
      }
    }
  }
  else {
    vertex_id_t localvid = global2localvid[vid];
    std::map<procid_t, 
            std::pair<block_synchronize_request2, std::vector<vertex_id_t>::iterator> > requests;
    // I should have all the in-edges. but I need the in vertices.
    // need to track the vertices added so I don't add duplicate vertices
    // if the vertex has both in-out edges to this vertex.
    // trick! vids are ordered!

    foreach(edge_id_t localineid, localstore.in_edge_ids(localvid)) {
      vertex_id_t localsourcevid = localstore.source(localineid);
      if (localvid_is_ghost(localsourcevid)) {
        // need to synchronize incoming vertex
        procid_t targetowner = localvid2owner[localsourcevid];
        block_synchronize_request2 &req = requests[targetowner].first;
        req.vid.push_back(local2globalvid[localsourcevid]);
        req.vidversion.push_back(localstore.vertex_version(localsourcevid));
        vertex_conditional_store vs;
        vs.hasdata = localstore.vertex_modified(localsourcevid);
        if (vs.hasdata) {
          localstore.set_vertex_modified(localsourcevid, false);
          vs.data.first = localstore.vertex_data(localsourcevid);
          vs.data.second = 0; // unused
        }
        requests[targetowner].second=req.vid.end();
        req.vstore.push_back(vs);
      }
    }
    // now for the out edges
    foreach(edge_id_t localouteid, localstore.out_edge_ids(localvid)) {
      vertex_id_t localtargetvid = localstore.target(localouteid);
      procid_t targetowner = localvid2owner[localstore.target(localouteid)];
      block_synchronize_request2 &req = requests[targetowner].first;

      if (localvid_is_ghost(localtargetvid)) {
        // need to synchronize outgoing vertex and outgoing edge
        // do outgoing vertex first
        if (std::binary_search(req.vid.begin(), requests[targetowner].second, local2globalvid[localtargetvid]) == false) {
          req.vid.push_back(local2globalvid[localtargetvid]);
          req.vidversion.push_back(localstore.vertex_version(localtargetvid));
          vertex_conditional_store vs;
          vs.hasdata = localstore.vertex_modified(localtargetvid);
          if (vs.hasdata) {
            localstore.set_vertex_modified(localtargetvid, false);
            vs.data.first = localstore.vertex_data(localtargetvid);
            vs.data.second = 0; // unused
          }
          req.vstore.push_back(vs);
        }
        // now for the outgoing edge
        
        req.srcdest.push_back(std::pair<vertex_id_t, vertex_id_t>(vid, local2globalvid[localtargetvid]));
        req.edgeversion.push_back(localstore.edge_version(localouteid));
        edge_conditional_store es;
        es.hasdata = localstore.edge_modified(localouteid);
        if (es.hasdata) {
          localstore.set_edge_modified(localouteid, false);
          es.data.first = localstore.edge_data(localouteid);
          es.data.second = 0;
        }
        req.estore.push_back(es);
      }
    }
    if (async == false) {
    
      typename std::map<procid_t, 
            std::pair<block_synchronize_request2, std::vector<vertex_id_t>::iterator> >::iterator iter;
            
      iter = requests.begin();
      while(iter != requests.end()) {
        iter->second.first = rmi.remote_request(iter->first,
                                          &distributed_graph<VertexData, EdgeData>::get_alot2,
                                          iter->second.first);
        // unpack
        update_alot2(iter->second.first);
        ++iter;
      }
    }
    else {
      typename std::map<procid_t, 
            std::pair<block_synchronize_request2, std::vector<vertex_id_t>::iterator> >::iterator iter;
      iter = requests.begin();
      while(iter != requests.end()) {
        pending_async_updates.inc();
        rmi.remote_call(iter->first,
                         &distributed_graph<VertexData, EdgeData>::async_get_alot2,
                         rmi.procid(),
                         iter->second.first);
        ++iter;
      }
    }
  }
}




/**
 * Waits for all asynchronous data synchronizations to complete
 */
template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::wait_for_all_async_syncs() {
  while(pending_async_updates.value != 0) sched_yield();
}



template <typename VertexData, typename EdgeData> 
typename distributed_graph<VertexData, EdgeData>::vertex_conditional_store 
distributed_graph<VertexData, EdgeData>::get_vertex_if_version_less_than(vertex_id_t vid,
                                                         uint64_t  vertexversion,
                                                         vertex_conditional_store &vdata) {
  vertex_conditional_store ret;
  size_t localvid = global2localvid[vid];
  uint64_t local_vertex_version = localstore.vertex_version(localvid);

  //logstream(LOG_DEBUG) << "get vertex: " << vid << ":" << vertexversion << " vs " << local_vertex_version << std::endl;
  
  if (local_vertex_version  >= vertexversion) {
    ret.hasdata = true;
    ret.data.first = localstore.vertex_data(localvid);
    ret.data.second = local_vertex_version;
  }
  else if (local_vertex_version < vertexversion) {
    assert(vdata.hasdata);
    localstore.vertex_data(localvid) = vdata.data.first;
    localstore.set_vertex_version(localvid, vertexversion);
    ret.hasdata = false;
  }
  return ret;
}

template <typename VertexData, typename EdgeData> 
typename distributed_graph<VertexData, EdgeData>::edge_conditional_store 
distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than(edge_id_t eid, 
                                                     uint64_t edgeversion,
                                                     edge_conditional_store &edata) {
  edge_conditional_store ret;
  size_t localeid = global2localeid[eid];
  uint64_t  local_edge_version = localstore.edge_version(localeid);
  
  //logstream(LOG_DEBUG) << "get edge: " << eid << ":" << edgeversion << " vs " << local_edge_version << std::endl;
  
  if (local_edge_version >= edgeversion) {
    ret.hasdata = true;
    ret.data.first = localstore.edge_data(localeid);
    ret.data.second = local_edge_version;
  }
  else if (local_edge_version < edgeversion) {
    assert(edata.hasdata);
    localstore.edge_data(localeid) = edata.data.first;
    localstore.set_edge_version(localeid, edgeversion);
    ret.hasdata = false;
  }
  return ret;
}

template <typename VertexData, typename EdgeData> 
typename distributed_graph<VertexData, EdgeData>::edge_conditional_store 
distributed_graph<VertexData, EdgeData>::get_edge_if_version_less_than2(vertex_id_t source,
                                                      vertex_id_t target,
                                                      uint64_t  edgeversion,
                                                      edge_conditional_store &edata) {
  edge_conditional_store ret;
  size_t localsource = global2localvid[source];
  size_t localtarget = global2localvid[target];


  std::pair<bool, edge_id_t> findret = localstore.find(localsource, localtarget);
  assert(findret.first);
  edge_id_t localeid = findret.second;
  
  uint64_t  local_edge_version = localstore.edge_version(localeid);

  //logstream(LOG_DEBUG) << "get edge2: " << "(" << localsource << "->" << localtarget << ")" << ":" << edgeversion << " vs " << local_edge_version << std::endl;
  if (local_edge_version >= edgeversion) {
    ret.hasdata = true;
    ret.data.first = localstore.edge_data(localeid);
    ret.data.second = local_edge_version;
  }
  else if (local_edge_version < edgeversion) {
    assert(edata.hasdata);
    localstore.edge_data(localeid) = edata.data.first;
    localstore.set_edge_version(localeid, edgeversion);
    ret.hasdata = false;
  }
  return ret;
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::async_get_vertex_if_version_less_than(
                                                              procid_t srcproc, 
                                                              vertex_id_t vid, 
                                                              uint64_t vertexversion,
                                                              vertex_conditional_store &vdata) {
  rmi.remote_call(srcproc,
                  &distributed_graph<VertexData, EdgeData>::reply_vertex_data_and_version,
                  vid,
                  get_vertex_if_version_less_than(vid, vertexversion, vdata));
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than(
                                                        procid_t srcproc, 
                                                        edge_id_t eid, 
                                                        uint64_t edgeversion,
                                                        edge_conditional_store &edata) {
  rmi.remote_call(srcproc,
                  &distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version,
                  eid,
                  get_edge_if_version_less_than(eid, edgeversion, edata));
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::async_get_edge_if_version_less_than2(
                                              procid_t srcproc, 
                                              vertex_id_t source, 
                                              vertex_id_t target, 
                                              uint64_t edgeversion,
                                              edge_conditional_store &edata) {
  rmi.remote_call(srcproc,
                  &distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version2,
                  source,
                  target,
                  get_edge_if_version_less_than2(source, target, edgeversion, edata));
}


template <typename VertexData, typename EdgeData> 
typename distributed_graph<VertexData, EdgeData>::block_synchronize_request& 
distributed_graph<VertexData, EdgeData>::get_alot(
                    distributed_graph<VertexData, EdgeData>::block_synchronize_request &request) {
  std::vector<vertex_conditional_store> vresponse(request.vid.size());
  std::vector<edge_conditional_store> eresponse(request.eid.size());
  for (size_t i = 0;i < request.vid.size(); ++i) {
    request.vstore[i] = get_vertex_if_version_less_than(request.vid[i], 
                                                        request.vidversion[i], 
                                                        request.vstore[i]);
  }
  for (size_t i = 0;i < request.eid.size(); ++i) {
    request.estore[i] = get_edge_if_version_less_than(request.eid[i], 
                                                      request.edgeversion[i], 
                                                      request.estore[i]);
  }
  request.vidversion.clear();
  request.edgeversion.clear();

  return request;
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::async_get_alot(
                procid_t srcproc,
                distributed_graph<VertexData, EdgeData>::block_synchronize_request &request) {
  get_alot(request);
  rmi.remote_call(srcproc,
                  &distributed_graph<VertexData, EdgeData>::reply_alot,
                  request);
}


template <typename VertexData, typename EdgeData> 
typename distributed_graph<VertexData, EdgeData>::block_synchronize_request2& 
distributed_graph<VertexData, EdgeData>::get_alot2(
                          distributed_graph<VertexData, EdgeData>::block_synchronize_request2 &request) {
  std::vector<vertex_conditional_store> vresponse(request.vid.size());
  std::vector<edge_conditional_store> eresponse(request.srcdest.size());
  for (size_t i = 0;i < request.vid.size(); ++i) {
    request.vstore[i] = get_vertex_if_version_less_than(request.vid[i], 
                                                        request.vidversion[i], 
                                                        request.vstore[i]);
  }
  for (size_t i = 0;i < request.srcdest.size(); ++i) {
    request.estore[i] = get_edge_if_version_less_than2(request.srcdest[i].first, 
                                                       request.srcdest[i].second, 
                                                       request.edgeversion[i], 
                                                       request.estore[i]);
  }
  request.vidversion.clear();
  request.edgeversion.clear();
  return request;
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::async_get_alot2(
                    procid_t srcproc,
                    distributed_graph<VertexData, EdgeData>::block_synchronize_request2 &request) {
  get_alot2(request);
  rmi.remote_call(srcproc,
                  &distributed_graph<VertexData, EdgeData>::reply_alot2,
                  request);
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::reply_vertex_data_and_version(
                        vertex_id_t vid, 
                        distributed_graph<VertexData, EdgeData>::vertex_conditional_store &vstore) {
  update_vertex_data_and_version(vid, vstore);
  pending_async_updates.dec();
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version(
                          edge_id_t eid, 
                          distributed_graph<VertexData, EdgeData>::edge_conditional_store &estore) {
  update_edge_data_and_version(eid, estore);
  pending_async_updates.dec();
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::reply_edge_data_and_version2(
                  vertex_id_t source, 
                  vertex_id_t target, 
                  distributed_graph<VertexData, EdgeData>::edge_conditional_store &estore) {
  update_edge_data_and_version2(source, target, estore);
  pending_async_updates.dec();
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::update_vertex_data_and_version(
                        vertex_id_t vid, 
                        distributed_graph<VertexData, EdgeData>::vertex_conditional_store &vstore) {
  if (vstore.hasdata) {
    vertex_id_t localvid = global2localvid[vid];
    localstore.vertex_data(localvid) = vstore.data.first;
    localstore.set_vertex_version(localvid, vstore.data.second);
  }
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::update_edge_data_and_version(
                           edge_id_t eid, 
                           distributed_graph<VertexData, EdgeData>::edge_conditional_store &estore) {
  if (estore.hasdata) {
    edge_id_t localeid = global2localeid[eid];
    localstore.edge_data(localeid) = estore.data.first;
    localstore.set_edge_version(localeid, estore.data.second);
  }
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::update_edge_data_and_version2(
                          vertex_id_t source, 
                          vertex_id_t target, 
                          distributed_graph<VertexData, EdgeData>::edge_conditional_store &estore) {
  if (estore.hasdata) {
    vertex_id_t localsourcevid = global2localvid[source];
    vertex_id_t localtargetvid = global2localvid[target];
    std::pair<bool, edge_id_t> findret = localstore.find(localsourcevid, localtargetvid);
    assert(findret.first);
    localstore.edge_data(findret.second) = estore.data.first;
    localstore.set_edge_version(findret.second, estore.data.second);
  }
}

template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::update_alot(
                  distributed_graph<VertexData, EdgeData>::block_synchronize_request &request) {
  for (size_t i = 0;i < request.vid.size(); ++i) {
    update_vertex_data_and_version(request.vid[i], request.vstore[i]);
  }

  for (size_t i = 0;i < request.eid.size(); ++i) {
    update_edge_data_and_version(request.eid[i], request.estore[i]);
  }
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::reply_alot(
            distributed_graph<VertexData, EdgeData>::block_synchronize_request &request) {
  update_alot(request);
  pending_async_updates.dec();
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::update_alot2(
                  distributed_graph<VertexData, EdgeData>::block_synchronize_request2 &request) {
  for (size_t i = 0;i < request.vid.size(); ++i) {
    update_vertex_data_and_version(request.vid[i], request.vstore[i]);
  }

  for (size_t i = 0;i < request.srcdest.size(); ++i) {
    update_edge_data_and_version2(request.srcdest[i].first, request.srcdest[i].second, request.estore[i]);
  }
}


template <typename VertexData, typename EdgeData> 
void distributed_graph<VertexData, EdgeData>::reply_alot2(
                    distributed_graph<VertexData, EdgeData>::block_synchronize_request2 &request) {
  update_alot2(request);
  pending_async_updates.dec();
}


template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_all_vertices(bool async) {
  foreach(vertex_id_t vid, ghostvertices) {
    synchronize_vertex(vid, async);
  }
}

template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_all_edges(bool async) {
  foreach(vertex_id_t vid, ghostvertices) {
    foreach(edge_id_t eid, localstore.in_edge_ids(global2localvid[vid])) {
      synchronize_edge(local2globaleid[eid], async);
    }
  }
}

template <typename VertexData, typename EdgeData>
void distributed_graph<VertexData, EdgeData>::synchronize_all_scopes(bool async) {
  foreach(vertex_id_t vid, boundaryscopes) {
    synchronize_scope(vid, async);
  }
}


#endif

