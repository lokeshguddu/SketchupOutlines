// Copyright 2013 Trimble Navigation Limited. All Rights Reserved.

#include <string>
#include <vector>
#include <cassert>

#include "./xmlexporter.h"
#include "./xmltexturehelper.h"
#include "../../common/xmlgeomutils.h"
#include "../../common/utils.h"

#include <slapi/import_export/pluginprogresscallback.h>
#include <slapi/initialize.h>
#include <slapi/model/component_definition.h>
#include <slapi/model/component_instance.h>
#include <slapi/model/drawing_element.h>
#include <slapi/model/edge.h>
#include <slapi/model/entities.h>
#include <slapi/model/entity.h>
#include <slapi/model/face.h>
#include <slapi/model/group.h>
#include <slapi/model/layer.h>
#include <slapi/model/loop.h>
#include <slapi/model/material.h>
#include <slapi/model/mesh_helper.h>
#include <slapi/model/model.h>
#include <slapi/model/texture.h>
#include <slapi/model/texture_writer.h>
#include <slapi/model/uv_helper.h>
#include <slapi/model/vertex.h>

using namespace XmlGeomUtils;

// A simple SUStringRef wrapper class which makes usage simpler from C++.
class CSUString {
 public:
  CSUString() {
    SUSetInvalid(su_str_);
    SUStringCreate(&su_str_);
  }

  ~CSUString() {
    SUStringRelease(&su_str_);
  }

  operator SUStringRef*() {
    return &su_str_;
  }

  std::string utf8() {
    size_t length;
    SUStringGetUTF8Length(su_str_, &length);
    std::string string;
    string.resize(length+1);
    size_t returned_length;
    SUStringGetUTF8(su_str_, length, &string[0], &returned_length);
    return string;
  }

private:
  // Disallow copying for simplicity
  CSUString(const CSUString& copy);
  CSUString& operator= (const CSUString& copy);

  SUStringRef su_str_;
};

// Utility function to get a material's name
static std::string GetMaterialName(SUMaterialRef material) {
  CSUString name;
  SU_CALL(SUMaterialGetName(material, name));
  return name.utf8();
}

// Utility function to get a layer's name
static std::string GetLayerName(SULayerRef layer) {
  CSUString name;
  SU_CALL(SULayerGetName(layer, name));
  return name.utf8();
}

// Utility function to get a component definition's name
static std::string GetComponentDefinitionName(
    SUComponentDefinitionRef comp_def) {
  CSUString name;
  SU_CALL(SUComponentDefinitionGetName(comp_def, name));
  return name.utf8();
}

CXmlExporter::CXmlExporter() {
  SUSetInvalid(model_);
  SUSetInvalid(texture_writer_);
}

CXmlExporter::~CXmlExporter() {
}

void CXmlExporter::ReleaseModelObjects() {
  if (!SUIsInvalid(texture_writer_)) {
    SUTextureWriterRelease(&texture_writer_);
    SUSetInvalid(texture_writer_);
  }

  if (!SUIsInvalid(model_)) {
    SUModelRelease(&model_);
    SUSetInvalid(model_);
  }

  // Terminate the SDK
  SUTerminate();
}

bool CXmlExporter::Convert(const std::string& src_file,
    const std::string& dst_file,
    SketchUpPluginProgressCallback* progress_callback) {
  bool exported = false;
  try {
    // Initialize the SDK
    SUInitialize();

    // Create the model from the src_file
    SUSetInvalid(model_);
    SU_CALL(SUModelCreateFromFile(&model_, src_file.c_str()));

    // Create a texture writer
    SUSetInvalid(texture_writer_);
    SU_CALL(SUTextureWriterCreate(&texture_writer_));

    // Open the xml file for creation
    if (!file_.Open(dst_file, true)) {
      ReleaseModelObjects();
      return exported;
    }

    // Write textures
    HandleProgress(progress_callback, 0.0, "Writing Texture Files...");
    WriteTextureFiles();

    // Write file header
    int major_ver = 0, minor_ver = 0, build_no = 0;
    SU_CALL(SUModelGetVersion(model_, &major_ver, &minor_ver, &build_no));
    file_.WriteHeader(major_ver, minor_ver, build_no);

    // Layers
    HandleProgress(progress_callback, 10.0, "Writing Layers...");
    WriteLayers();

    // Materials
    HandleProgress(progress_callback, 20.0, "Writing Materials...");
    WriteMaterials();

    // Component definitions
//    HandleProgress(progress_callback, 40.0, "Writing Definitions...");
//    WriteComponentDefinitions();

    // Geometry
    HandleProgress(progress_callback, 60.0, "Writing Geometry...");
    WriteGeometry();

    file_.Close(IsCancelled(progress_callback));

    HandleProgress(progress_callback, 100.0, "Export Complete");
    exported = true;
  } catch(...) {
    exported = false;
    file_.Close(true);
  }
  ReleaseModelObjects();

  return exported;
}

void CXmlExporter::WriteTextureFiles() {
  if (options_.export_materials()) {
    // Load the textures into the texture writer
    CXmlTextureHelper texture_helper;
    size_t texture_count = texture_helper.LoadAllTextures(model_,
        texture_writer_,
        options_.export_materials_by_layer());
    stats_.set_textures(texture_count);

    // Write out all the textures to a the export folder  
    if (texture_count > 0) {
      std::string texture_directory = file_.GetTextureDirectory();
      SU_CALL(SUTextureWriterWriteAllTextures(texture_writer_,
                                              texture_directory.c_str()));
    }
  }
}

void CXmlExporter::WriteLayers() {
  if (options_.export_layers()) {
    file_.StartLayers();

    // Get the number of layers
    size_t num_layers = 0;
    SU_CALL(SUModelGetNumLayers(model_, &num_layers));
    if (num_layers > 0) {
      // Get the layers
      std::vector<SULayerRef> layers(num_layers);
      SU_CALL(SUModelGetLayers(model_, num_layers, &layers[0], &num_layers));
      // Write out each layer
      for (size_t i = 0; i < num_layers; i++) {
        SULayerRef layer = layers[i];
        WriteLayer(layer);
      }
    }

    file_.PopParentNode();
  }
}

static XmlMaterialInfo GetMaterialInfo(SUMaterialRef material) {
  assert(!SUIsInvalid(material));

  XmlMaterialInfo info;

  // Name
  info.name_ = GetMaterialName(material);

  // Color
  info.has_color_ = false;
  info.has_alpha_ = false;
  SUMaterialType type;
  SU_CALL(SUMaterialGetType(material, &type));
  // Color
  if ((type == SUMaterialType_Colored) ||
      (type == SUMaterialType_ColorizedTexture)) {
    SUColor color;
    if (SUMaterialGetColor(material, &color) == SU_ERROR_NONE) {
      info.has_color_ = true;
      info.color_ = color;
    }
  }

  // Alpha
  bool has_alpha = false;
  SU_CALL(SUMaterialGetUseOpacity(material, &has_alpha));
  if (has_alpha) {
    double alpha = 0;
    SU_CALL(SUMaterialGetOpacity(material, &alpha));
    info.has_alpha_ = true;
    info.alpha_ = alpha;
  }

  // Texture
  info.has_texture_ = false;
  if ((type == SUMaterialType_Textured) ||
      (type == SUMaterialType_ColorizedTexture)) {
    SUTextureRef texture = SU_INVALID;
    if (SUMaterialGetTexture(material, &texture) == SU_ERROR_NONE) {
      info.has_texture_ = true;
      // Texture path
      CSUString texture_path;
      SU_CALL(SUTextureGetFileName(texture, texture_path));
      info.texture_path_ = texture_path.utf8();

      // Texture scale
      size_t width = 0;
      size_t height = 0;
      double s_scale = 0.0;
      double t_scale = 0.0;
      SU_CALL(SUTextureGetDimensions(texture, &width, &height,
                                     &s_scale, &t_scale));
      info.texture_sscale_ = s_scale;
      info.texture_tscale_ = t_scale;
    }
  }

  return info;
}

void CXmlExporter::WriteLayer(SULayerRef layer) {
  if (SUIsInvalid(layer))
    return;

  XmlLayerInfo info;

  // Name
  info.name_ = GetLayerName(layer);

  // Color
  SUMaterialRef material = SU_INVALID;
  info.has_material_info_ = false;
  if (SULayerGetMaterial(layer, &material) == SU_ERROR_NONE) {
    info.has_material_info_ = true;
    info.material_info_ = GetMaterialInfo(material);
  }

  // Visibility
  bool is_visible = true;
  SU_CALL(SULayerGetVisibility(layer, &is_visible));
  info.is_visible_ = is_visible;

  stats_.AddLayer();
  file_.WriteLayerInfo(info);
}

void CXmlExporter::WriteMaterials() {
  if (options_.export_materials()) {
    if (options_.export_materials_by_layer()) {
      size_t num_layers;
      SU_CALL(SUModelGetNumLayers(model_, &num_layers));
      if (num_layers > 0) {
        std::vector<SULayerRef> layers(num_layers);
        SU_CALL(SUModelGetLayers(model_, num_layers, &layers[0], &num_layers));
        file_.StartMaterials();
        for (size_t i = 0; i < num_layers; i++)  {
          SULayerRef layer = layers[i];
          SUMaterialRef material = SU_INVALID;
          if (SULayerGetMaterial(layer, &material) == SU_ERROR_NONE) {
            WriteMaterial(material);
          }
        }
        file_.PopParentNode();
      }
    } else {
      size_t count = 0;
      SU_CALL(SUModelGetNumMaterials(model_, &count));
      if (count > 0) {
        file_.StartMaterials();
        std::vector<SUMaterialRef> materials(count);
        SU_CALL(SUModelGetMaterials(model_, count, &materials[0], &count));
        for (size_t i=0; i<count; i++) {
          WriteMaterial(materials[i]);
        }
        file_.PopParentNode();
      }
    }
  }
}

void CXmlExporter::WriteMaterial(SUMaterialRef material) {
  if (SUIsInvalid(material))
    return;

  XmlMaterialInfo info = GetMaterialInfo(material);
  file_.WriteMaterialInfo(info);
}

void CXmlExporter::WriteGeometry() {
  if (options_.export_faces() || options_.export_edges()) {
    // Write entities
    SUEntitiesRef model_entities;
    SU_CALL(SUModelGetEntities(model_, &model_entities));
    file_.StartGeometry();
    WriteEntities(model_entities);
    file_.PopParentNode();
  }
}

void CXmlExporter::WriteComponentDefinitions() {
  size_t num_comp_defs = 0;
  SU_CALL(SUModelGetNumComponentDefinitions(model_, &num_comp_defs));
  if (num_comp_defs > 0) {
    file_.StartComponentDefinitions();

    std::vector<SUComponentDefinitionRef> comp_defs(num_comp_defs);
    SU_CALL(SUModelGetComponentDefinitions(model_, num_comp_defs, &comp_defs[0],
                                           &num_comp_defs));
    for (size_t def = 0; def < num_comp_defs; ++def) {
      SUComponentDefinitionRef comp_def = comp_defs[def];
      WriteComponentDefinition(comp_def);
    }

    file_.PopParentNode();
  }
}

void CXmlExporter::WriteComponentDefinition(SUComponentDefinitionRef comp_def) {
  std::string name = GetComponentDefinitionName(comp_def);
  file_.StartComponentDefinition(name);

  SUEntitiesRef entities = SU_INVALID;
  SUComponentDefinitionGetEntities(comp_def, &entities);
  WriteEntities(entities);

  file_.PopParentNode();
}

void CXmlExporter::WriteEntities(SUEntitiesRef entities) {
  // Component instances
  size_t num_instances = 0;
  SU_CALL(SUEntitiesGetNumInstances(entities, &num_instances));
  if (num_instances > 0) {
    std::vector<SUComponentInstanceRef> instances(num_instances);
    SU_CALL(SUEntitiesGetInstances(entities, num_instances,
                                   &instances[0], &num_instances));
    for (size_t c = 0; c < num_instances; c++) {
      SUComponentInstanceRef instance = instances[c];
      SUComponentDefinitionRef definition = SU_INVALID;
      SU_CALL(SUComponentInstanceGetDefinition(instance, &definition));

      XmlComponentInstanceInfo instance_info;
      
      // Layer
      SULayerRef layer = SU_INVALID;
      SUDrawingElementGetLayer(SUComponentInstanceToDrawingElement(instance),
                               &layer);
      if (!SUIsInvalid(layer))
        instance_info.layer_name_ = GetLayerName(layer);

      // Material
      SUMaterialRef material = SU_INVALID;
      SUDrawingElementGetMaterial(SUComponentInstanceToDrawingElement(instance),
                                  &material);
      if (!SUIsInvalid(material))
        instance_info.material_name_ = GetMaterialName(material);

      instance_info.definition_name_ = GetComponentDefinitionName(definition);
      SU_CALL(SUComponentInstanceGetTransform(instance,
                                              &instance_info.transform_));
      file_.WriteComponentInstanceInfo(instance_info);
    }
  }

  // Groups
  size_t num_groups = 0;
  SU_CALL(SUEntitiesGetNumGroups(entities, &num_groups));
  if (num_groups > 0) {
    std::vector<SUGroupRef> groups(num_groups);
    SU_CALL(SUEntitiesGetGroups(entities, num_groups, &groups[0], &num_groups));
    for (size_t g = 0; g < num_groups; g++) {
      SUGroupRef group = groups[g];
      SUComponentDefinitionRef group_component = SU_INVALID;
      SUEntitiesRef group_entities = SU_INVALID;
      SU_CALL(SUGroupGetEntities(group, &group_entities));
      inheritance_manager_.PushElement(group);
      file_.StartGroup();

      // Write entities
      WriteEntities(group_entities);

      // Write transformation
      SUTransformation transform;
      SU_CALL(SUGroupGetTransform(group, &transform));
      file_.WriteTransformation(transform);

      file_.PopParentNode();
      inheritance_manager_.PopElement();
    }
  }

  // Faces
  if (options_.export_faces()) {
    size_t num_faces = 0;
    SU_CALL(SUEntitiesGetNumFaces(entities, &num_faces));
    if (num_faces > 0) {
      std::vector<SUFaceRef> faces(num_faces);
      SU_CALL(SUEntitiesGetFaces(entities, num_faces, &faces[0], &num_faces));
      for (size_t i = 0; i < num_faces; i++) {
        inheritance_manager_.PushElement(faces[i]);
        WriteFace(faces[i]);
        inheritance_manager_.PopElement();
      }
    }
  }

  // Edges
  //if (options_.export_edges()) {
//    size_t num_edges = 0;
//    bool standAloneOnly = true; // Write only edges not connected to faces.
//    SU_CALL(SUEntitiesGetNumEdges(entities, standAloneOnly, &num_edges));
//    if (num_edges > 0) {
//      std::vector<SUEdgeRef> edges(num_edges);
//      SU_CALL(SUEntitiesGetEdges(entities, standAloneOnly, num_edges,
//                                 &edges[0], &num_edges));
//      for (size_t i = 0; i < num_edges; i++) {
//        inheritance_manager_.PushElement(edges[i]);
//        WriteEdge(edges[i]);
//        inheritance_manager_.PopElement();
//      }
//    }
  //}

  // Curves
//  //if (options_.export_edges()) {
//    size_t num_curves = 0;
//    SU_CALL(SUEntitiesGetNumCurves(entities, &num_curves));
//    if (num_curves > 0) {
//      std::vector<SUCurveRef> curves(num_curves);
//      SU_CALL(SUEntitiesGetCurves(entities, num_curves,
//                                  &curves[0], &num_curves));
//      for (size_t i = 0; i < num_curves; i++) {
//        WriteCurve(curves[i]);
//      }
//    }
//  //}
}

void CXmlExporter::WriteFace(SUFaceRef face) {
  if (SUIsInvalid(face))
    return;


  //outer loop
    XmlFaceInfo info;
    info.has_single_loop_ = true;
    SULoopRef outer_loop = SU_INVALID;
    SU_CALL(SUFaceGetOuterLoop(face, &outer_loop));
    size_t num_vertices;
    SU_CALL(SULoopGetNumVertices(outer_loop, &num_vertices));
    if (num_vertices > 0) {
        std::vector<SUVertexRef> vertices(num_vertices);
        SU_CALL(SULoopGetVertices(outer_loop, num_vertices, &vertices[0],&num_vertices));
        for (size_t i = 0; i < num_vertices; i++) {
            XmlFaceVertex vertex_info;
            // vertex position
            SUPoint3D su_point;
            SUVertexRef vertex_ref = vertices[i];
            SU_CALL(SUVertexGetPosition(vertex_ref, &su_point));
            vertex_info.vertex_ = CPoint3d(su_point);
            
            info.vertices_.push_back(vertex_info);
        }
    }
    stats_.AddFace();
    file_.WriteFaceInfo(info);
    
   
    
    //Inner loop
    
    size_t num_loops = 0;
    SU_CALL(SUFaceGetNumInnerLoops(face, &num_loops));
    if(num_loops >0)
    {
        std::vector<SULoopRef> loops(num_loops);
        SU_CALL(SUFaceGetInnerLoops(face, num_loops, &loops[0], &num_loops));
        for(size_t i=0;i<num_loops;i++)
        {
            XmlFaceInfo info;
            info.has_single_loop_ = true;
            SULoopRef inner_loop = loops[i];
            size_t num_vertices;
            SU_CALL(SULoopGetNumVertices(inner_loop, &num_vertices));
            if (num_vertices > 0) {
                std::vector<SUVertexRef> vertices(num_vertices);
                SU_CALL(SULoopGetVertices(inner_loop, num_vertices, &vertices[0],&num_vertices));
                for (size_t j = 0; j < num_vertices; j++) {
                    XmlFaceVertex vertex_info;
                    // vertex position
                    SUPoint3D su_point;
                    SUVertexRef vertex_ref = vertices[j];
                    SU_CALL(SUVertexGetPosition(vertex_ref, &su_point));
                    vertex_info.vertex_ = CPoint3d(su_point);
                    
                    info.vertices_.push_back(vertex_info);
                }
            }
            stats_.AddFace();
            file_.WriteFaceInfo(info);
        }
    }
}

XmlEdgeInfo CXmlExporter::GetEdgeInfo(SUEdgeRef edge) const {
  XmlEdgeInfo info;
  info.has_layer_ = false;
  if (options_.export_layers()) {
    info.has_layer_ = true;
    SULayerRef layer = inheritance_manager_.GetCurrentLayer();
    if (!SUIsInvalid(layer)) {
      SU_CALL(SUDrawingElementGetLayer(SUEdgeToDrawingElement(edge), &layer));
      info.layer_name_ = GetLayerName(layer);
    }
  }

  // Edge color
  info.has_color_ = false;
  if (options_.export_materials()) {
    info.color_ = inheritance_manager_.GetCurrentEdgeColor();
    info.has_color_ = true;
  }

  info.start_ = CPoint3d(-1, -1, -1);
  SUVertexRef start_vertex = SU_INVALID;
  SU_CALL(SUEdgeGetStartVertex(edge, &start_vertex));
  SUPoint3D p;
  SU_CALL(SUVertexGetPosition(start_vertex, &p));
  info.start_ = CPoint3d(p);

  info.end_ = CPoint3d(-1, -1, -1);
  SUVertexRef end_vertex = SU_INVALID;
  SU_CALL(SUEdgeGetEndVertex(edge, &end_vertex));
  SU_CALL(SUVertexGetPosition(end_vertex, &p));
  info.end_ = CPoint3d(p);

  return info;
}

void CXmlExporter::WriteEdge(SUEdgeRef edge) {
  if (SUIsInvalid(edge))
    return;

  XmlEdgeInfo info = GetEdgeInfo(edge);
  file_.WriteEdgeInfo(info);
  stats_.AddEdge();
}

void CXmlExporter::WriteCurve(SUCurveRef curve) {
  if (SUIsInvalid(curve))
    return;

  XmlCurveInfo info;
  size_t num_edges = 0;
  SU_CALL(SUCurveGetNumEdges(curve, &num_edges));
  std::vector<SUEdgeRef> edges(num_edges);
  SU_CALL(SUCurveGetEdges(curve, num_edges, &edges[0], &num_edges));
  for (size_t i = 0; i < num_edges; ++i) {
    XmlEdgeInfo edge_info = GetEdgeInfo(edges[i]);
    info.edges_.push_back(edge_info);
  }
  file_.WriteCurveInfo(info);
}
