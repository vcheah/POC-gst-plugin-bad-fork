/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "gstd3d11memory.h"
#include "gstd3d11device.h"
#include "gstd3d11utils.h"

GST_DEBUG_CATEGORY_STATIC (gst_d3d11_allocator_debug);
#define GST_CAT_DEFAULT gst_d3d11_allocator_debug

#define GST_D3D11_ALLOCATOR_GET_LOCK(a) (&(GST_D3D11_ALLOCATOR_CAST(a)->priv->lock))
#define GST_D3D11_ALLOCATOR_LOCK(a) g_mutex_lock(GST_D3D11_ALLOCATOR_GET_LOCK(a))
#define GST_D3D11_ALLOCATOR_UNLOCK(a) g_mutex_unlock(GST_D3D11_ALLOCATOR_GET_LOCK(a))

#define GST_D3D11_MEMORY_CAST(m) ((GstD3D11Memory *) m)
#define GST_D3D11_MEMORY_GET_LOCK(m) (&(GST_D3D11_MEMORY_CAST(m)->priv->lock))
#define GST_D3D11_MEMORY_LOCK(m) g_mutex_lock(GST_D3D11_MEMORY_GET_LOCK(m))
#define GST_D3D11_MEMORY_UNLOCK(m) g_mutex_unlock(GST_D3D11_MEMORY_GET_LOCK(m))

#define GST_D3D11_MEMORY_NAME "D3D11Memory"

/**
 * gst_d3d11_allocation_params_new:
 * @device: a #GstD3D11Device
 * @info: a #GstVideoInfo
 * @flags: a #GstD3D11AllocationFlags
 * @bind_flags: D3D11_BIND_FLAG value used for creating Direct3D11 texture
 *
 * Create #GstD3D11AllocationParams object which is used by #GstD3D11BufferPool
 * and #GstD3D11Allocator in order to allocate new ID3D11Texture2D
 * object with given configuration
 *
 * Returns: a #GstD3D11AllocationParams or %NULL if @info is not supported
 *
 * Since: 1.20
 */
GstD3D11AllocationParams *
gst_d3d11_allocation_params_new (GstD3D11Device * device, GstVideoInfo * info,
    GstD3D11AllocationFlags flags, guint bind_flags)
{
  GstD3D11AllocationParams *ret;
  const GstD3D11Format *d3d11_format;
  gint i;

  g_return_val_if_fail (info != NULL, NULL);

  d3d11_format = gst_d3d11_device_format_from_gst (device,
      GST_VIDEO_INFO_FORMAT (info));
  if (!d3d11_format) {
    GST_WARNING ("Couldn't get d3d11 format");
    return NULL;
  }

  ret = g_new0 (GstD3D11AllocationParams, 1);

  ret->info = *info;
  ret->aligned_info = *info;
  ret->d3d11_format = d3d11_format;

  /* Usage Flag
   * https://docs.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_usage
   *
   * +----------------------------------------------------------+
   * | Resource Usage | Default | Dynamic | Immutable | Staging |
   * +----------------+---------+---------+-----------+---------+
   * | GPU-Read       | Yes     | Yes     | Yes       | Yes     |
   * | GPU-Write      | Yes     |         |           | Yes     |
   * | CPU-Read       |         |         |           | Yes     |
   * | CPU-Write      |         | Yes     |           | Yes     |
   * +----------------------------------------------------------+
   */

  /* If corresponding dxgi format is undefined, use resource format instead */
  if (d3d11_format->dxgi_format == DXGI_FORMAT_UNKNOWN) {
    for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
      g_assert (d3d11_format->resource_format[i] != DXGI_FORMAT_UNKNOWN);

      ret->desc[i].Width = GST_VIDEO_INFO_COMP_WIDTH (info, i);
      ret->desc[i].Height = GST_VIDEO_INFO_COMP_HEIGHT (info, i);
      ret->desc[i].MipLevels = 1;
      ret->desc[i].ArraySize = 1;
      ret->desc[i].Format = d3d11_format->resource_format[i];
      ret->desc[i].SampleDesc.Count = 1;
      ret->desc[i].SampleDesc.Quality = 0;
      ret->desc[i].Usage = D3D11_USAGE_DEFAULT;
      ret->desc[i].BindFlags = bind_flags;
    }
  } else {
    ret->desc[0].Width = GST_VIDEO_INFO_WIDTH (info);
    ret->desc[0].Height = GST_VIDEO_INFO_HEIGHT (info);
    ret->desc[0].MipLevels = 1;
    ret->desc[0].ArraySize = 1;
    ret->desc[0].Format = d3d11_format->dxgi_format;
    ret->desc[0].SampleDesc.Count = 1;
    ret->desc[0].SampleDesc.Quality = 0;
    ret->desc[0].Usage = D3D11_USAGE_DEFAULT;
    ret->desc[0].BindFlags = bind_flags;
  }

  ret->flags = flags;

  return ret;
}

/**
 * gst_d3d11_allocation_params_alignment:
 * @params: a #GstD3D11AllocationParams
 * @align: a #GstVideoAlignment
 *
 * Adjust Width and Height fields of D3D11_TEXTURE2D_DESC with given
 * @align
 *
 * Returns: %TRUE if alignment could be applied
 *
 * Since: 1.20
 */
gboolean
gst_d3d11_allocation_params_alignment (GstD3D11AllocationParams * params,
    GstVideoAlignment * align)
{
  gint i;
  guint padding_width, padding_height;
  GstVideoInfo *info;
  GstVideoInfo new_info;

  g_return_val_if_fail (params != NULL, FALSE);
  g_return_val_if_fail (align != NULL, FALSE);

  /* d3d11 does not support stride align. Consider padding only */
  padding_width = align->padding_left + align->padding_right;
  padding_height = align->padding_top + align->padding_bottom;

  info = &params->info;

  if (!gst_video_info_set_format (&new_info, GST_VIDEO_INFO_FORMAT (info),
          GST_VIDEO_INFO_WIDTH (info) + padding_width,
          GST_VIDEO_INFO_HEIGHT (info) + padding_height)) {
    GST_WARNING ("Set format fail");
    return FALSE;
  }

  params->aligned_info = new_info;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    params->desc[i].Width = GST_VIDEO_INFO_COMP_WIDTH (&new_info, i);
    params->desc[i].Height = GST_VIDEO_INFO_COMP_HEIGHT (&new_info, i);
  }

  return TRUE;
}

/**
 * gst_d3d11_allocation_params_copy:
 * @src: a #GstD3D11AllocationParams
 *
 * Returns: a copy of @src
 *
 * Since: 1.20
 */
GstD3D11AllocationParams *
gst_d3d11_allocation_params_copy (GstD3D11AllocationParams * src)
{
  GstD3D11AllocationParams *dst;

  g_return_val_if_fail (src != NULL, NULL);

  dst = g_new0 (GstD3D11AllocationParams, 1);
  memcpy (dst, src, sizeof (GstD3D11AllocationParams));

  return dst;
}

/**
 * gst_d3d11_allocation_params_free:
 * @params: a #GstD3D11AllocationParams
 *
 * Free @params
 *
 * Since: 1.20
 */
void
gst_d3d11_allocation_params_free (GstD3D11AllocationParams * params)
{
  g_free (params);
}

static gint
gst_d3d11_allocation_params_compare (const GstD3D11AllocationParams * p1,
    const GstD3D11AllocationParams * p2)
{
  g_return_val_if_fail (p1 != NULL, -1);
  g_return_val_if_fail (p2 != NULL, -1);

  if (p1 == p2)
    return 0;

  return -1;
}

static void
_init_alloc_params (GType type)
{
  static GstValueTable table = {
    0, (GstValueCompareFunc) gst_d3d11_allocation_params_compare,
    NULL, NULL
  };

  table.type = type;
  gst_value_register (&table);
}

G_DEFINE_BOXED_TYPE_WITH_CODE (GstD3D11AllocationParams,
    gst_d3d11_allocation_params,
    (GBoxedCopyFunc) gst_d3d11_allocation_params_copy,
    (GBoxedFreeFunc) gst_d3d11_allocation_params_free,
    _init_alloc_params (g_define_type_id));

typedef enum
{
  GST_D3D11_MEMORY_TYPE_TEXTURE = 0,
  GST_D3D11_MEMORY_TYPE_ARRAY = 1,
  GST_D3D11_MEMORY_TYPE_STAGING = 2,
} GstD3D11MemoryType;

struct _GstD3D11MemoryPrivate
{
  GstD3D11MemoryType type;

  ID3D11Texture2D *texture;
  D3D11_TEXTURE2D_DESC desc;

  /* valid only for array typed memory */
  guint subresource_index;

  ID3D11Texture2D *staging;

  ID3D11ShaderResourceView *shader_resource_view[GST_VIDEO_MAX_PLANES];
  guint num_shader_resource_views;

  ID3D11RenderTargetView *render_target_view[GST_VIDEO_MAX_PLANES];
  guint num_render_target_views;

  ID3D11VideoDecoderOutputView *decoder_output_view;
  ID3D11VideoProcessorInputView *processor_input_view;
  ID3D11VideoProcessorOutputView *processor_output_view;

  D3D11_MAPPED_SUBRESOURCE map;

  GMutex lock;
  gint cpu_map_count;
};

struct _GstD3D11AllocatorPrivate
{
  /* parent texture when array typed memory is used */
  ID3D11Texture2D *texture;
  GArray *array_in_use;
  GArray *decoder_output_view_array;
  GArray *processor_input_view_array;

  /* Count the number of array textures in use */
  guint num_array_textures_in_use;
  guint array_texture_size;

  GMutex lock;
  GCond cond;

  gboolean flushing;
};

#define gst_d3d11_allocator_parent_class parent_class
G_DEFINE_TYPE_WITH_PRIVATE (GstD3D11Allocator,
    gst_d3d11_allocator, GST_TYPE_ALLOCATOR);

static inline D3D11_MAP
gst_map_flags_to_d3d11 (GstMapFlags flags)
{
  if ((flags & GST_MAP_READWRITE) == GST_MAP_READWRITE)
    return D3D11_MAP_READ_WRITE;
  else if ((flags & GST_MAP_WRITE) == GST_MAP_WRITE)
    return D3D11_MAP_WRITE;
  else if ((flags & GST_MAP_READ) == GST_MAP_READ)
    return D3D11_MAP_READ;
  else
    g_assert_not_reached ();

  return D3D11_MAP_READ;
}

static ID3D11Texture2D *
create_staging_texture (GstD3D11Device * device,
    const D3D11_TEXTURE2D_DESC * ref)
{
  D3D11_TEXTURE2D_DESC desc = { 0, };
  ID3D11Texture2D *texture = NULL;
  ID3D11Device *device_handle = gst_d3d11_device_get_device_handle (device);
  HRESULT hr;

  desc.Width = ref->Width;
  desc.Height = ref->Height;
  desc.MipLevels = 1;
  desc.Format = ref->Format;
  desc.SampleDesc.Count = 1;
  desc.ArraySize = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE);

  hr = ID3D11Device_CreateTexture2D (device_handle, &desc, NULL, &texture);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR_OBJECT (device, "Failed to create texture");
    return NULL;
  }

  return texture;
}

static gboolean
map_cpu_access_data (GstD3D11Memory * dmem, D3D11_MAP map_type)
{
  GstD3D11MemoryPrivate *priv = dmem->priv;
  HRESULT hr;
  gboolean ret = TRUE;
  ID3D11Resource *texture = (ID3D11Resource *) priv->texture;
  ID3D11Resource *staging = (ID3D11Resource *) priv->staging;
  ID3D11DeviceContext *device_context =
      gst_d3d11_device_get_device_context_handle (dmem->device);

  gst_d3d11_device_lock (dmem->device);
  if (GST_MEMORY_FLAG_IS_SET (dmem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD)) {
    ID3D11DeviceContext_CopySubresourceRegion (device_context,
        staging, 0, 0, 0, 0, texture, priv->subresource_index, NULL);
  }

  hr = ID3D11DeviceContext_Map (device_context,
      staging, 0, map_type, 0, &priv->map);

  if (!gst_d3d11_result (hr, dmem->device)) {
    GST_ERROR_OBJECT (GST_MEMORY_CAST (dmem)->allocator,
        "Failed to map staging texture (0x%x)", (guint) hr);
    ret = FALSE;
  }

  gst_d3d11_device_unlock (dmem->device);

  return ret;
}

static gpointer
gst_d3d11_memory_map_staging (GstMemory * mem, GstMapFlags flags)
{
  GstD3D11Memory *dmem = (GstD3D11Memory *) mem;
  GstD3D11MemoryPrivate *priv = dmem->priv;

  GST_D3D11_MEMORY_LOCK (dmem);
  if (priv->cpu_map_count == 0) {
    ID3D11DeviceContext *device_context =
        gst_d3d11_device_get_device_context_handle (dmem->device);
    D3D11_MAP map_type;
    HRESULT hr;
    gboolean ret = TRUE;

    map_type = gst_map_flags_to_d3d11 (flags);

    gst_d3d11_device_lock (dmem->device);
    hr = ID3D11DeviceContext_Map (device_context,
        (ID3D11Resource *) priv->texture, 0, map_type, 0, &priv->map);
    if (!gst_d3d11_result (hr, dmem->device)) {
      GST_ERROR_OBJECT (GST_MEMORY_CAST (dmem)->allocator,
          "Failed to map staging texture (0x%x)", (guint) hr);
      ret = FALSE;
    }
    gst_d3d11_device_unlock (dmem->device);

    if (!ret) {
      GST_D3D11_MEMORY_UNLOCK (dmem);
      return NULL;
    }
  }

  priv->cpu_map_count++;
  GST_D3D11_MEMORY_UNLOCK (dmem);

  return dmem->priv->map.pData;
}

static gpointer
gst_d3d11_memory_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GstD3D11Memory *dmem = (GstD3D11Memory *) mem;
  GstD3D11MemoryPrivate *priv = dmem->priv;

  if (priv->type == GST_D3D11_MEMORY_TYPE_STAGING) {
    if ((flags & GST_MAP_D3D11) == GST_MAP_D3D11)
      return priv->texture;

    return gst_d3d11_memory_map_staging (mem, flags);
  }

  GST_D3D11_MEMORY_LOCK (dmem);
  if ((flags & GST_MAP_D3D11) == GST_MAP_D3D11) {
    if (priv->staging &&
        GST_MEMORY_FLAG_IS_SET (dmem, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD)) {
      ID3D11DeviceContext *device_context =
          gst_d3d11_device_get_device_context_handle (dmem->device);

      gst_d3d11_device_lock (dmem->device);
      ID3D11DeviceContext_CopySubresourceRegion (device_context,
          (ID3D11Resource *) priv->texture, priv->subresource_index, 0, 0, 0,
          (ID3D11Resource *) priv->staging, 0, NULL);
      gst_d3d11_device_unlock (dmem->device);
    }

    GST_MEMORY_FLAG_UNSET (dmem, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD);

    if ((flags & GST_MAP_WRITE) == GST_MAP_WRITE)
      GST_MINI_OBJECT_FLAG_SET (dmem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);

    g_assert (priv->texture != NULL);
    GST_D3D11_MEMORY_UNLOCK (dmem);

    return priv->texture;
  }

  if (priv->cpu_map_count == 0) {
    D3D11_MAP map_type;

    /* Allocate staging texture for CPU access */
    if (!priv->staging) {
      priv->staging = create_staging_texture (dmem->device, &priv->desc);
      if (!priv->staging) {
        GST_ERROR_OBJECT (mem->allocator, "Couldn't create staging texture");
        GST_D3D11_MEMORY_UNLOCK (dmem);

        return NULL;
      }

      /* first memory, always need download to staging */
      GST_MINI_OBJECT_FLAG_SET (mem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);
    }

    map_type = gst_map_flags_to_d3d11 (flags);

    if (!map_cpu_access_data (dmem, map_type)) {
      GST_ERROR_OBJECT (mem->allocator, "Couldn't map staging texture");
      GST_D3D11_MEMORY_UNLOCK (dmem);

      return NULL;
    }
  }

  if ((flags & GST_MAP_WRITE) == GST_MAP_WRITE)
    GST_MINI_OBJECT_FLAG_SET (mem, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD);

  GST_MEMORY_FLAG_UNSET (mem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);

  priv->cpu_map_count++;
  GST_D3D11_MEMORY_UNLOCK (dmem);

  return dmem->priv->map.pData;
}

static void
unmap_cpu_access_data (GstD3D11Memory * dmem)
{
  GstD3D11MemoryPrivate *priv = dmem->priv;
  ID3D11Resource *staging = (ID3D11Resource *) priv->staging;
  ID3D11DeviceContext *device_context =
      gst_d3d11_device_get_device_context_handle (dmem->device);

  if (priv->type == GST_D3D11_MEMORY_TYPE_STAGING)
    staging = (ID3D11Resource *) priv->texture;

  gst_d3d11_device_lock (dmem->device);
  ID3D11DeviceContext_Unmap (device_context, staging, 0);
  gst_d3d11_device_unlock (dmem->device);
}

static void
gst_d3d11_memory_unmap_full (GstMemory * mem, GstMapInfo * info)
{
  GstD3D11Memory *dmem = (GstD3D11Memory *) mem;
  GstD3D11MemoryPrivate *priv = dmem->priv;

  GST_D3D11_MEMORY_LOCK (dmem);
  if ((info->flags & GST_MAP_D3D11) == GST_MAP_D3D11) {
    if (priv->type != GST_D3D11_MEMORY_TYPE_STAGING &&
        (info->flags & GST_MAP_WRITE) == GST_MAP_WRITE)
      GST_MINI_OBJECT_FLAG_SET (mem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);

    GST_D3D11_MEMORY_UNLOCK (dmem);
    return;
  }

  if (priv->type != GST_D3D11_MEMORY_TYPE_STAGING &&
      (info->flags & GST_MAP_WRITE))
    GST_MINI_OBJECT_FLAG_SET (mem, GST_D3D11_MEMORY_TRANSFER_NEED_UPLOAD);

  priv->cpu_map_count--;
  if (priv->cpu_map_count > 0) {
    GST_D3D11_MEMORY_UNLOCK (dmem);
    return;
  }

  unmap_cpu_access_data (dmem);

  GST_D3D11_MEMORY_UNLOCK (dmem);
}

static GstMemory *
gst_d3d11_memory_share (GstMemory * mem, gssize offset, gssize size)
{
  /* TODO: impl. */
  return NULL;
}

static GstMemory *
gst_d3d11_allocator_dummy_alloc (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  g_return_val_if_reached (NULL);
}

static void
gst_d3d11_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstD3D11Allocator *self = GST_D3D11_ALLOCATOR (allocator);
  GstD3D11AllocatorPrivate *priv = self->priv;
  GstD3D11Memory *dmem = (GstD3D11Memory *) mem;
  GstD3D11MemoryPrivate *dmem_priv = dmem->priv;
  gint i;

  if (priv->array_in_use) {
    GST_D3D11_ALLOCATOR_LOCK (self);
    g_array_index (priv->array_in_use,
        guint8, dmem_priv->subresource_index) = 0;
    priv->num_array_textures_in_use--;
    g_cond_broadcast (&priv->cond);
    GST_D3D11_ALLOCATOR_UNLOCK (self);
  }

  for (i = 0; i < GST_VIDEO_MAX_PLANES; i++) {
    if (dmem_priv->render_target_view[i])
      ID3D11RenderTargetView_Release (dmem_priv->render_target_view[i]);

    if (dmem_priv->shader_resource_view[i])
      ID3D11ShaderResourceView_Release (dmem_priv->shader_resource_view[i]);
  }

  if (dmem_priv->decoder_output_view)
    ID3D11VideoDecoderOutputView_Release (dmem_priv->decoder_output_view);

  if (dmem_priv->processor_input_view)
    ID3D11VideoProcessorInputView_Release (dmem_priv->processor_input_view);

  if (dmem_priv->processor_output_view)
    ID3D11VideoProcessorOutputView_Release (dmem_priv->processor_output_view);

  if (dmem_priv->texture)
    ID3D11Texture2D_Release (dmem_priv->texture);

  if (dmem_priv->staging)
    ID3D11Texture2D_Release (dmem_priv->staging);

  gst_clear_object (&dmem->device);
  g_mutex_clear (&dmem_priv->lock);
  g_free (dmem->priv);
  g_free (dmem);
}

static void
gst_d3d11_allocator_dispose (GObject * object)
{
  GstD3D11Allocator *alloc = GST_D3D11_ALLOCATOR (object);
  GstD3D11AllocatorPrivate *priv = alloc->priv;

  g_clear_pointer (&priv->decoder_output_view_array, g_array_unref);
  g_clear_pointer (&priv->processor_input_view_array, g_array_unref);

  if (priv->texture) {
    ID3D11Texture2D_Release (priv->texture);
    priv->texture = NULL;
  }

  gst_clear_object (&alloc->device);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_d3d11_allocator_finalize (GObject * object)
{
  GstD3D11Allocator *alloc = GST_D3D11_ALLOCATOR (object);
  GstD3D11AllocatorPrivate *priv = alloc->priv;

  g_mutex_clear (&priv->lock);
  g_cond_clear (&priv->cond);

  g_clear_pointer (&priv->array_in_use, g_array_unref);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_d3d11_allocator_class_init (GstD3D11AllocatorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS (klass);

  gobject_class->dispose = gst_d3d11_allocator_dispose;
  gobject_class->finalize = gst_d3d11_allocator_finalize;

  allocator_class->alloc = gst_d3d11_allocator_dummy_alloc;
  allocator_class->free = gst_d3d11_allocator_free;

  GST_DEBUG_CATEGORY_INIT (gst_d3d11_allocator_debug, "d3d11allocator", 0,
      "d3d11allocator object");
}

static void
gst_d3d11_allocator_init (GstD3D11Allocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);
  GstD3D11AllocatorPrivate *priv;

  alloc->mem_type = GST_D3D11_MEMORY_NAME;
  alloc->mem_map = gst_d3d11_memory_map;
  alloc->mem_unmap_full = gst_d3d11_memory_unmap_full;
  alloc->mem_share = gst_d3d11_memory_share;
  /* fallback copy */

  GST_OBJECT_FLAG_SET (alloc, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);

  priv = gst_d3d11_allocator_get_instance_private (allocator);
  g_mutex_init (&priv->lock);
  g_cond_init (&priv->cond);
  priv->array_texture_size = 1;

  allocator->priv = priv;
}

/**
 * gst_d3d11_allocator_new:
 * @device: a #GstD3D11Device
 *
 * Returns: a newly created #GstD3D11Allocator
 *
 * Since: 1.20
 */
GstD3D11Allocator *
gst_d3d11_allocator_new (GstD3D11Device * device)
{
  GstD3D11Allocator *allocator;

  g_return_val_if_fail (GST_IS_D3D11_DEVICE (device), NULL);

  allocator = g_object_new (GST_TYPE_D3D11_ALLOCATOR, NULL);
  allocator->device = gst_object_ref (device);

  return allocator;
}

static gboolean
calculate_mem_size (GstD3D11Device * device, ID3D11Texture2D * texture,
    const D3D11_TEXTURE2D_DESC * desc, D3D11_MAP map_type,
    gint stride[GST_VIDEO_MAX_PLANES], gsize * size)
{
  HRESULT hr;
  gboolean ret = TRUE;
  D3D11_MAPPED_SUBRESOURCE map;
  gsize offset[GST_VIDEO_MAX_PLANES];
  ID3D11DeviceContext *device_context =
      gst_d3d11_device_get_device_context_handle (device);

  gst_d3d11_device_lock (device);
  hr = ID3D11DeviceContext_Map (device_context,
      (ID3D11Resource *) texture, 0, map_type, 0, &map);

  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR_OBJECT (device, "Failed to map texture (0x%x)", (guint) hr);
    gst_d3d11_device_unlock (device);
    return FALSE;
  }

  ret = gst_d3d11_dxgi_format_get_size (desc->Format,
      desc->Width, desc->Height, map.RowPitch, offset, stride, size);

  ID3D11DeviceContext_Unmap (device_context, (ID3D11Resource *) texture, 0);
  gst_d3d11_device_unlock (device);

  return ret;
}

static gboolean
create_shader_resource_views (GstD3D11Memory * mem)
{
  GstD3D11MemoryPrivate *priv = mem->priv;
  gint i;
  HRESULT hr;
  guint num_views = 0;
  ID3D11Device *device_handle;
  D3D11_SHADER_RESOURCE_VIEW_DESC resource_desc = { 0, };
  DXGI_FORMAT formats[GST_VIDEO_MAX_PLANES] = { DXGI_FORMAT_UNKNOWN, };

  device_handle = gst_d3d11_device_get_device_handle (mem->device);

  switch (priv->desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      num_views = 1;
      formats[0] = priv->desc.Format;
      break;
    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_YUY2:
      num_views = 1;
      formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      break;
    case DXGI_FORMAT_NV12:
      num_views = 2;
      formats[0] = DXGI_FORMAT_R8_UNORM;
      formats[1] = DXGI_FORMAT_R8G8_UNORM;
      break;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
      num_views = 2;
      formats[0] = DXGI_FORMAT_R16_UNORM;
      formats[1] = DXGI_FORMAT_R16G16_UNORM;
      break;
    case DXGI_FORMAT_Y210:
      num_views = 1;
      formats[0] = DXGI_FORMAT_R16G16B16A16_UNORM;
      break;
    case DXGI_FORMAT_Y410:
      num_views = 1;
      formats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
      break;
    default:
      g_assert_not_reached ();
      return FALSE;
  }

  if ((priv->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ==
      D3D11_BIND_SHADER_RESOURCE) {
    resource_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    resource_desc.Texture2D.MipLevels = 1;

    for (i = 0; i < num_views; i++) {
      resource_desc.Format = formats[i];
      hr = ID3D11Device_CreateShaderResourceView (device_handle,
          (ID3D11Resource *) priv->texture, &resource_desc,
          &priv->shader_resource_view[i]);

      if (!gst_d3d11_result (hr, mem->device)) {
        GST_ERROR_OBJECT (GST_MEMORY_CAST (mem)->allocator,
            "Failed to create %dth resource view (0x%x)", i, (guint) hr);
        goto error;
      }
    }

    priv->num_shader_resource_views = num_views;

    return TRUE;
  }

  return FALSE;

error:
  for (i = 0; i < num_views; i++) {
    if (priv->shader_resource_view[i])
      ID3D11ShaderResourceView_Release (priv->shader_resource_view[i]);
    priv->shader_resource_view[i] = NULL;
  }

  priv->num_shader_resource_views = 0;

  return FALSE;
}

static gboolean
create_render_target_views (GstD3D11Memory * mem)
{
  GstD3D11MemoryPrivate *priv = mem->priv;
  gint i;
  HRESULT hr;
  guint num_views = 0;
  ID3D11Device *device_handle;
  D3D11_RENDER_TARGET_VIEW_DESC render_desc = { 0, };
  DXGI_FORMAT formats[GST_VIDEO_MAX_PLANES] = { DXGI_FORMAT_UNKNOWN, };

  device_handle = gst_d3d11_device_get_device_handle (mem->device);

  switch (priv->desc.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
      num_views = 1;
      formats[0] = priv->desc.Format;
      break;
    case DXGI_FORMAT_AYUV:
      num_views = 1;
      formats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      break;
    case DXGI_FORMAT_NV12:
      num_views = 2;
      formats[0] = DXGI_FORMAT_R8_UNORM;
      formats[1] = DXGI_FORMAT_R8G8_UNORM;
      break;
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
      num_views = 2;
      formats[0] = DXGI_FORMAT_R16_UNORM;
      formats[1] = DXGI_FORMAT_R16G16_UNORM;
      break;
    default:
      g_assert_not_reached ();
      return FALSE;
  }

  if ((priv->desc.BindFlags & D3D11_BIND_RENDER_TARGET) ==
      D3D11_BIND_RENDER_TARGET) {
    render_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    render_desc.Texture2D.MipSlice = 0;

    for (i = 0; i < num_views; i++) {
      render_desc.Format = formats[i];

      hr = ID3D11Device_CreateRenderTargetView (device_handle,
          (ID3D11Resource *) priv->texture, &render_desc,
          &priv->render_target_view[i]);
      if (!gst_d3d11_result (hr, mem->device)) {
        GST_ERROR_OBJECT (GST_MEMORY_CAST (mem)->allocator,
            "Failed to create %dth render target view (0x%x)", i, (guint) hr);
        goto error;
      }
    }

    priv->num_render_target_views = num_views;

    return TRUE;
  }

  return FALSE;

error:
  for (i = 0; i < num_views; i++) {
    if (priv->render_target_view[i])
      ID3D11RenderTargetView_Release (priv->render_target_view[i]);
    priv->render_target_view[i] = NULL;
  }

  priv->num_render_target_views = 0;

  return FALSE;
}

static void
gst_d3d11_decoder_output_view_clear (ID3D11VideoDecoderOutputView ** view)
{
  if (view && *view) {
    ID3D11VideoDecoderOutputView_Release (*view);
    *view = NULL;
  }
}

static void
gst_d3d11_processor_input_view_clear (ID3D11VideoProcessorInputView ** view)
{
  if (view && *view) {
    ID3D11VideoProcessorInputView_Release (*view);
    *view = NULL;
  }
}

static gboolean
check_bind_flags_for_processor_input_view (guint bind_flags)
{
  static const guint compatible_flags = (D3D11_BIND_DECODER |
      D3D11_BIND_VIDEO_ENCODER | D3D11_BIND_RENDER_TARGET |
      D3D11_BIND_UNORDERED_ACCESS);

  if (bind_flags == 0)
    return TRUE;

  if ((bind_flags & compatible_flags) != 0)
    return TRUE;

  return FALSE;
}

/**
 * gst_d3d11_allocator_alloc:
 * @allocator: a #GstD3D11Allocator
 * @desc: a D3D11_TEXTURE2D_DESC struct
 * @flags: a #GstD3D11AllocationFlags
 * @size: a size of CPU accesible memory
 *
 * Returns: a newly allocated #GstD3D11Memory with given parameters.
 *
 * Since: 1.20
 */
GstMemory *
gst_d3d11_allocator_alloc (GstD3D11Allocator * allocator,
    const D3D11_TEXTURE2D_DESC * desc, GstD3D11AllocationFlags flags,
    gsize size)
{
  GstD3D11Memory *mem;
  GstD3D11Device *device;
  ID3D11Texture2D *texture = NULL;
  guint index_to_use = 0;
  GstD3D11AllocatorPrivate *priv;
  GstD3D11MemoryType type = GST_D3D11_MEMORY_TYPE_TEXTURE;
  HRESULT hr;
  ID3D11Device *device_handle;

  g_return_val_if_fail (GST_IS_D3D11_ALLOCATOR (allocator), NULL);
  g_return_val_if_fail (desc != NULL, NULL);
  g_return_val_if_fail (size > 0, NULL);

  priv = allocator->priv;
  device = allocator->device;
  device_handle = gst_d3d11_device_get_device_handle (device);

  if ((flags & GST_D3D11_ALLOCATION_FLAG_TEXTURE_ARRAY)) {
    gint i;

  do_again:
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    if (priv->flushing) {
      GST_DEBUG_OBJECT (allocator, "we are flushing");
      GST_D3D11_ALLOCATOR_UNLOCK (allocator);

      return NULL;
    }

    if (!priv->array_in_use) {
      priv->array_in_use = g_array_sized_new (FALSE,
          TRUE, sizeof (guint8), desc->ArraySize);
      g_array_set_size (priv->array_in_use, desc->ArraySize);

      priv->array_texture_size = desc->ArraySize;

      if ((desc->BindFlags & D3D11_BIND_DECODER) == D3D11_BIND_DECODER &&
          !priv->decoder_output_view_array) {
        priv->decoder_output_view_array = g_array_sized_new (FALSE,
            TRUE, sizeof (ID3D11VideoDecoderOutputView *), desc->ArraySize);
        g_array_set_clear_func (priv->decoder_output_view_array,
            (GDestroyNotify) gst_d3d11_decoder_output_view_clear);
        g_array_set_size (priv->decoder_output_view_array, desc->ArraySize);
      }

      if (check_bind_flags_for_processor_input_view (desc->BindFlags)) {
        priv->processor_input_view_array = g_array_sized_new (FALSE,
            TRUE, sizeof (ID3D11VideoProcessorInputView *), desc->ArraySize);
        g_array_set_clear_func (priv->processor_input_view_array,
            (GDestroyNotify) gst_d3d11_processor_input_view_clear);
        g_array_set_size (priv->processor_input_view_array, desc->ArraySize);
      }
    }

    for (i = 0; i < desc->ArraySize; i++) {
      if (!g_array_index (priv->array_in_use, guint8, i)) {
        index_to_use = i;
        break;
      }
    }

    if (i == desc->ArraySize) {
      GST_DEBUG_OBJECT (allocator, "All elements in array are used now");
      g_cond_wait (&priv->cond, &priv->lock);
      GST_D3D11_ALLOCATOR_UNLOCK (allocator);
      goto do_again;
    }

    g_array_index (priv->array_in_use, guint8, index_to_use) = 1;
    priv->num_array_textures_in_use++;

    GST_D3D11_ALLOCATOR_UNLOCK (allocator);

    if (!priv->texture) {
      hr = ID3D11Device_CreateTexture2D (device_handle, desc, NULL,
          &priv->texture);
      if (!gst_d3d11_result (hr, device)) {
        GST_ERROR_OBJECT (allocator, "Couldn't create texture");
        goto error;
      }
    }

    ID3D11Texture2D_AddRef (priv->texture);
    texture = priv->texture;

    type = GST_D3D11_MEMORY_TYPE_ARRAY;
  } else {
    hr = ID3D11Device_CreateTexture2D (device_handle, desc, NULL, &texture);
    if (!gst_d3d11_result (hr, device)) {
      GST_ERROR_OBJECT (allocator, "Couldn't create texture");
      goto error;
    }
  }

  mem = g_new0 (GstD3D11Memory, 1);
  mem->priv = g_new0 (GstD3D11MemoryPrivate, 1);

  gst_memory_init (GST_MEMORY_CAST (mem),
      0, GST_ALLOCATOR_CAST (allocator), NULL, size, 0, 0, size);

  g_mutex_init (&mem->priv->lock);
  mem->priv->texture = texture;
  mem->priv->desc = *desc;
  mem->priv->type = type;
  mem->priv->subresource_index = index_to_use;
  mem->device = gst_object_ref (device);

  return GST_MEMORY_CAST (mem);

error:
  if (texture)
    ID3D11Texture2D_Release (texture);

  return NULL;
}

/**
 * gst_d3d11_allocator_alloc_staging:
 * @allocator: a #GstD3D11Allocator
 * @desc: a D3D11_TEXTURE2D_DESC struct
 * @flags: a #GstD3D11AllocationFlags
 * @stride: (out): a stride of CPU accesible memory
 *
 * Returns: a newly allocated #GstD3D11Memory with given parameters.
 * Returned #GstD3D11Memory can be used only for staging texture.
 *
 * Since: 1.20
 */
GstMemory *
gst_d3d11_allocator_alloc_staging (GstD3D11Allocator * allocator,
    const D3D11_TEXTURE2D_DESC * desc, GstD3D11AllocationFlags flags,
    gint * stride)
{
  GstD3D11Memory *mem;
  GstD3D11Device *device;
  ID3D11Texture2D *texture = NULL;
  gsize mem_size = 0;
  gint mem_stride[GST_VIDEO_MAX_PLANES];

  g_return_val_if_fail (GST_IS_D3D11_ALLOCATOR (allocator), NULL);
  g_return_val_if_fail (desc != NULL, NULL);

  device = allocator->device;

  texture = create_staging_texture (device, desc);
  if (!texture) {
    GST_ERROR_OBJECT (allocator, "Couldn't create staging texture");
    goto error;
  }

  if (!calculate_mem_size (device,
          texture, desc, D3D11_MAP_READ, mem_stride, &mem_size)) {
    GST_ERROR_OBJECT (allocator, "Couldn't calculate staging texture size");
    goto error;
  }

  mem = g_new0 (GstD3D11Memory, 1);
  mem->priv = g_new0 (GstD3D11MemoryPrivate, 1);

  gst_memory_init (GST_MEMORY_CAST (mem),
      0, GST_ALLOCATOR_CAST (allocator), NULL, mem_size, 0, 0, mem_size);

  g_mutex_init (&mem->priv->lock);
  mem->priv->texture = texture;
  mem->priv->desc = *desc;
  mem->priv->type = GST_D3D11_MEMORY_TYPE_STAGING;
  mem->device = gst_object_ref (device);

  /* every plan will have identical size */
  if (stride)
    *stride = mem_stride[0];

  return GST_MEMORY_CAST (mem);

error:
  if (texture)
    ID3D11Texture2D_Release (texture);

  return NULL;
}

/**
 * gst_d3d11_allocator_set_flushing:
 * @allocator: a #GstD3D11Allocator
 * @flusing: whether to start or stop flusing
 *
 * Enable or disable the flushing state of @allocator.
 *
 * Since: 1.20
 */
void
gst_d3d11_allocator_set_flushing (GstD3D11Allocator * allocator,
    gboolean flushing)
{
  GstD3D11AllocatorPrivate *priv;

  g_return_if_fail (GST_IS_D3D11_ALLOCATOR (allocator));

  priv = allocator->priv;

  GST_D3D11_ALLOCATOR_LOCK (allocator);
  priv->flushing = flushing;
  g_cond_broadcast (&priv->cond);
  GST_D3D11_ALLOCATOR_UNLOCK (allocator);
}

/**
 * gst_d3d11_allocator_get_texture_array_size:
 * @allocator: a #GstD3D11Allocator
 * @array_size: (out) (optional): the size of texture array
 * @num_texture_in_use: (out) (optional): the number of textures in use
 *
 * Returns: %TRUE if the size of texture array is known
 *
 * Since: 1.20
 */
gboolean
gst_d3d11_allocator_get_texture_array_size (GstD3D11Allocator * allocator,
    guint * array_size, guint * num_texture_in_use)
{
  GstD3D11AllocatorPrivate *priv;

  g_return_val_if_fail (GST_IS_D3D11_ALLOCATOR (allocator), FALSE);

  priv = allocator->priv;

  /* For non-array-texture memory, the size is 1 */
  if (array_size)
    *array_size = priv->array_texture_size;
  if (num_texture_in_use)
    *num_texture_in_use = 1;

  /* size == 1 means we are not texture pool allocator */
  if (priv->array_texture_size == 1)
    return TRUE;

  if (num_texture_in_use) {
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    *num_texture_in_use = priv->num_array_textures_in_use;
    GST_D3D11_ALLOCATOR_UNLOCK (allocator);
  }

  return TRUE;
}

/**
 * gst_is_d3d11_memory:
 * @mem: a #GstMemory
 *
 * Returns: whether @mem is a #GstD3D11Memory
 *
 * Since: 1.20
 */
gboolean
gst_is_d3d11_memory (GstMemory * mem)
{
  return mem != NULL && mem->allocator != NULL &&
      GST_IS_D3D11_ALLOCATOR (mem->allocator);
}

/**
 * gst_d3d11_memory_get_texture_handle:
 * @mem: a #GstD3D11Memory
 *
 * Returns: (transfer none): a ID3D11Texture2D handle. Caller must not release
 * returned handle.
 *
 * Since: 1.20
 */
ID3D11Texture2D *
gst_d3d11_memory_get_texture_handle (GstD3D11Memory * mem)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);

  return mem->priv->texture;
}

/**
 * gst_d3d11_memory_get_subresource_index:
 * @mem: a #GstD3D11Memory
 *
 * Returns: subresource index corresponding to @mem.
 *
 * Since: 1.20
 */
guint
gst_d3d11_memory_get_subresource_index (GstD3D11Memory * mem)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), 0);

  return mem->priv->subresource_index;
}

/**
 * gst_d3d11_memory_get_texture_desc:
 * @mem: a #GstD3D11Memory
 * @desc: (out): a D3D11_TEXTURE2D_DESC
 *
 * Fill @desc with D3D11_TEXTURE2D_DESC for ID3D11Texture2D
 *
 * Returns: %TRUE if successeed
 *
 * Since: 1.20
 */
gboolean
gst_d3d11_memory_get_texture_desc (GstD3D11Memory * mem,
    D3D11_TEXTURE2D_DESC * desc)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), FALSE);
  g_return_val_if_fail (desc != NULL, FALSE);

  *desc = mem->priv->desc;

  return TRUE;
}

static gboolean
gst_d3d11_memory_ensure_shader_resource_view (GstD3D11Memory * mem)
{
  GstD3D11MemoryPrivate *priv = mem->priv;
  gboolean ret = FALSE;

  if (!(priv->desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
    GST_LOG_OBJECT (GST_MEMORY_CAST (mem)->allocator,
        "Need BindFlags, current flag 0x%x", priv->desc.BindFlags);
    return FALSE;
  }

  GST_D3D11_MEMORY_LOCK (mem);
  if (priv->num_shader_resource_views) {
    ret = TRUE;
    goto done;
  }

  ret = create_shader_resource_views (mem);

done:
  GST_D3D11_MEMORY_UNLOCK (mem);

  return ret;
}

/**
 * gst_d3d11_memory_get_shader_resource_view_size:
 * @mem: a #GstD3D11Memory
 *
 * Returns: the number of ID3D11ShaderResourceView that can be used
 * for processing GPU operation with @mem
 *
 * Since: 1.20
 */
guint
gst_d3d11_memory_get_shader_resource_view_size (GstD3D11Memory * mem)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), 0);

  if (!gst_d3d11_memory_ensure_shader_resource_view (mem))
    return 0;

  return mem->priv->num_shader_resource_views;
}

/**
 * gst_d3d11_memory_get_shader_resource_view:
 * @mem: a #GstD3D11Memory
 * @index: the index of the ID3D11ShaderResourceView
 *
 * Returns: (transfer none) (nullable): a pointer to the
 * ID3D11ShaderResourceView or %NULL if ID3D11ShaderResourceView is unavailable
 * for @index
 *
 * Since: 1.20
 */
ID3D11ShaderResourceView *
gst_d3d11_memory_get_shader_resource_view (GstD3D11Memory * mem, guint index)
{
  GstD3D11MemoryPrivate *priv;

  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);

  if (!gst_d3d11_memory_ensure_shader_resource_view (mem))
    return NULL;

  priv = mem->priv;

  if (index >= priv->num_shader_resource_views) {
    GST_ERROR ("Invalid SRV index %d", index);
    return NULL;
  }

  return priv->shader_resource_view[index];
}

static gboolean
gst_d3d11_memory_ensure_render_target_view (GstD3D11Memory * mem)
{
  GstD3D11MemoryPrivate *priv = mem->priv;
  gboolean ret = FALSE;

  if (!(priv->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
    GST_WARNING_OBJECT (GST_MEMORY_CAST (mem)->allocator,
        "Need BindFlags, current flag 0x%x", priv->desc.BindFlags);
    return FALSE;
  }

  GST_D3D11_MEMORY_LOCK (mem);
  if (priv->num_render_target_views) {
    ret = TRUE;
    goto done;
  }

  ret = create_render_target_views (mem);

done:
  GST_D3D11_MEMORY_UNLOCK (mem);

  return ret;
}

/**
 * gst_d3d11_memory_get_render_target_view_size:
 * @mem: a #GstD3D11Memory
 *
 * Returns: the number of ID3D11RenderTargetView that can be used
 * for processing GPU operation with @mem
 *
 * Since: 1.20
 */
guint
gst_d3d11_memory_get_render_target_view_size (GstD3D11Memory * mem)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), 0);

  if (!gst_d3d11_memory_ensure_render_target_view (mem))
    return 0;

  return mem->priv->num_render_target_views;
}

/**
 * gst_d3d11_memory_get_render_target_view:
 * @mem: a #GstD3D11Memory
 * @index: the index of the ID3D11RenderTargetView
 *
 * Returns: (transfer none) (nullable): a pointer to the
 * ID3D11RenderTargetView or %NULL if ID3D11RenderTargetView is unavailable
 * for @index
 *
 * Since: 1.20
 */
ID3D11RenderTargetView *
gst_d3d11_memory_get_render_target_view (GstD3D11Memory * mem, guint index)
{
  GstD3D11MemoryPrivate *priv;

  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);

  if (!gst_d3d11_memory_ensure_render_target_view (mem))
    return NULL;

  priv = mem->priv;

  if (index >= priv->num_render_target_views) {
    GST_ERROR ("Invalid RTV index %d", index);
    return NULL;
  }

  return priv->render_target_view[index];
}

static gboolean
gst_d3d11_memory_ensure_decoder_output_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device, GUID * decoder_profile)
{
  GstD3D11MemoryPrivate *dmem_priv = mem->priv;
  GstD3D11Allocator *allocator;
  GstD3D11AllocatorPrivate *priv;
  D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC desc;
  ID3D11VideoDecoderOutputView *view = NULL;
  HRESULT hr;
  gboolean ret = FALSE;

  allocator = GST_D3D11_ALLOCATOR (GST_MEMORY_CAST (mem)->allocator);
  priv = allocator->priv;

  if (!(dmem_priv->desc.BindFlags & D3D11_BIND_DECODER)) {
    GST_LOG_OBJECT (allocator,
        "Need BindFlags, current flag 0x%x", dmem_priv->desc.BindFlags);
    return FALSE;
  }

  GST_D3D11_MEMORY_LOCK (mem);
  if (dmem_priv->decoder_output_view) {
    ID3D11VideoDecoderOutputView_GetDesc (dmem_priv->decoder_output_view,
        &desc);
    if (IsEqualGUID (&desc.DecodeProfile, decoder_profile)) {
      goto succeeded;
    } else {
      /* Shouldn't happen, but try again anyway */
      GST_WARNING_OBJECT (allocator,
          "Existing view has different decoder profile");
      ID3D11VideoDecoderOutputView_Release (dmem_priv->decoder_output_view);
      dmem_priv->decoder_output_view = NULL;
    }
  }

  if (priv->decoder_output_view_array) {
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    view = g_array_index (priv->decoder_output_view_array,
        ID3D11VideoDecoderOutputView *, dmem_priv->subresource_index);

    if (view) {
      ID3D11VideoDecoderOutputView_GetDesc (view, &desc);
      /* Shouldn't happen because decoder will not reuse this allocator
       * over different codec/profiles */
      if (!IsEqualGUID (&desc.DecodeProfile, decoder_profile)) {
        GST_WARNING_OBJECT (allocator,
            "Existing view has different decoder profile");
        ID3D11VideoDecoderOutputView_Release (view);
        view = NULL;
        g_array_index (priv->decoder_output_view_array,
            ID3D11VideoDecoderOutputView *,
            dmem_priv->subresource_index) = NULL;
      } else {
        /* Increase refcount and reuse existing view */
        dmem_priv->decoder_output_view = view;
        ID3D11VideoDecoderOutputView_AddRef (view);
      }
    }
    GST_D3D11_ALLOCATOR_UNLOCK (allocator);
  }

  if (dmem_priv->decoder_output_view)
    goto succeeded;

  desc.DecodeProfile = *decoder_profile;
  desc.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
  desc.Texture2D.ArraySlice = dmem_priv->subresource_index;

  hr = ID3D11VideoDevice_CreateVideoDecoderOutputView (video_device,
      (ID3D11Resource *) dmem_priv->texture, &desc,
      &dmem_priv->decoder_output_view);
  if (!gst_d3d11_result (hr, mem->device)) {
    GST_ERROR_OBJECT (allocator,
        "Could not create decoder output view, hr: 0x%x", (guint) hr);
    goto done;
  }

  /* Store view array for later reuse */
  if (priv->decoder_output_view_array) {
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    view = g_array_index (priv->decoder_output_view_array,
        ID3D11VideoDecoderOutputView *, dmem_priv->subresource_index);

    if (view)
      ID3D11VideoDecoderOutputView_Release (view);

    g_array_index (priv->decoder_output_view_array,
        ID3D11VideoDecoderOutputView *, dmem_priv->subresource_index) =
        dmem_priv->decoder_output_view;
    ID3D11VideoDecoderOutputView_AddRef (dmem_priv->decoder_output_view);
    GST_D3D11_ALLOCATOR_UNLOCK (allocator);
  }

succeeded:
  ret = TRUE;

done:
  GST_D3D11_MEMORY_UNLOCK (mem);

  return ret;
}

/**
 * gst_d3d11_memory_get_decoder_output_view:
 * @mem: a #GstD3D11Memory
 *
 * Returns: (transfer none) (nullable): a pointer to the
 * ID3D11VideoDecoderOutputView or %NULL if ID3D11VideoDecoderOutputView is
 * unavailable
 *
 * Since: 1.20
 */
ID3D11VideoDecoderOutputView *
gst_d3d11_memory_get_decoder_output_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device, GUID * decoder_profile)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);
  g_return_val_if_fail (video_device != NULL, NULL);
  g_return_val_if_fail (decoder_profile != NULL, NULL);

  if (!gst_d3d11_memory_ensure_decoder_output_view (mem,
          video_device, decoder_profile))
    return NULL;

  return mem->priv->decoder_output_view;
}

static gboolean
gst_d3d11_memory_ensure_processor_input_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device,
    ID3D11VideoProcessorEnumerator * enumerator)
{
  GstD3D11MemoryPrivate *dmem_priv = mem->priv;
  GstD3D11Allocator *allocator;
  GstD3D11AllocatorPrivate *priv;
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;
  ID3D11VideoProcessorInputView *view = NULL;
  HRESULT hr;
  gboolean ret = FALSE;

  allocator = GST_D3D11_ALLOCATOR (GST_MEMORY_CAST (mem)->allocator);
  priv = allocator->priv;

  if (!check_bind_flags_for_processor_input_view (dmem_priv->desc.BindFlags)) {
    GST_LOG_OBJECT (allocator,
        "Need BindFlags, current flag 0x%x", dmem_priv->desc.BindFlags);
    return FALSE;
  }

  GST_D3D11_MEMORY_LOCK (mem);
  if (dmem_priv->processor_input_view)
    goto succeeded;

  if (priv->processor_input_view_array) {
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    view = g_array_index (priv->processor_input_view_array,
        ID3D11VideoProcessorInputView *, dmem_priv->subresource_index);

    /* Increase refcount and reuse existing view */
    if (view) {
      dmem_priv->processor_input_view = view;
      ID3D11VideoProcessorInputView_AddRef (view);
    }
    GST_D3D11_ALLOCATOR_UNLOCK (allocator);
  }

  if (dmem_priv->processor_input_view)
    goto succeeded;

  desc.FourCC = 0;
  desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = 0;
  desc.Texture2D.ArraySlice = dmem_priv->subresource_index;

  hr = ID3D11VideoDevice_CreateVideoProcessorInputView (video_device,
      (ID3D11Resource *) dmem_priv->texture, enumerator, &desc,
      &dmem_priv->processor_input_view);
  if (!gst_d3d11_result (hr, mem->device)) {
    GST_ERROR_OBJECT (allocator,
        "Could not create processor input view, hr: 0x%x", (guint) hr);
    goto done;
  }

  /* Store view array for later reuse */
  if (priv->processor_input_view_array) {
    GST_D3D11_ALLOCATOR_LOCK (allocator);
    view = g_array_index (priv->processor_input_view_array,
        ID3D11VideoProcessorInputView *, dmem_priv->subresource_index);

    if (view)
      ID3D11VideoProcessorInputView_Release (view);

    g_array_index (priv->processor_input_view_array,
        ID3D11VideoProcessorInputView *, dmem_priv->subresource_index) =
        dmem_priv->processor_input_view;
    ID3D11VideoProcessorInputView_AddRef (dmem_priv->processor_input_view);
    GST_D3D11_ALLOCATOR_UNLOCK (allocator);
  }

succeeded:
  ret = TRUE;

done:
  GST_D3D11_MEMORY_UNLOCK (mem);

  return ret;
}

/**
 * gst_d3d11_memory_get_processor_input_view:
 * @mem: a #GstD3D11Memory
 * @video_device: a #ID3D11VideoDevice
 * @enumerator: a #ID3D11VideoProcessorEnumerator
 *
 * Returns: (transfer none) (nullable): a pointer to the
 * ID3D11VideoProcessorInputView or %NULL if ID3D11VideoProcessorInputView is
 * unavailable
 *
 * Since: 1.20
 */
ID3D11VideoProcessorInputView *
gst_d3d11_memory_get_processor_input_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device,
    ID3D11VideoProcessorEnumerator * enumerator)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);
  g_return_val_if_fail (video_device != NULL, NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);

  if (!gst_d3d11_memory_ensure_processor_input_view (mem, video_device,
          enumerator))
    return NULL;

  return mem->priv->processor_input_view;
}

static gboolean
gst_d3d11_memory_ensure_processor_output_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device,
    ID3D11VideoProcessorEnumerator * enumerator)
{
  GstD3D11MemoryPrivate *priv = mem->priv;
  GstD3D11Allocator *allocator;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = { 0, };
  HRESULT hr;
  gboolean ret;

  allocator = GST_D3D11_ALLOCATOR (GST_MEMORY_CAST (mem)->allocator);

  if (!(priv->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
    GST_LOG_OBJECT (allocator,
        "Need BindFlags, current flag 0x%x", priv->desc.BindFlags);
    return FALSE;
  }

  /* FIXME: texture array should be supported at some point */
  if (priv->subresource_index != 0) {
    GST_FIXME_OBJECT (allocator,
        "Texture array is not suppoted for processor output view");
    return FALSE;
  }

  GST_D3D11_MEMORY_LOCK (mem);
  if (priv->processor_output_view)
    goto succeeded;

  desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipSlice = 0;

  hr = ID3D11VideoDevice_CreateVideoProcessorOutputView (video_device,
      (ID3D11Resource *) priv->texture, enumerator, &desc,
      &priv->processor_output_view);
  if (!gst_d3d11_result (hr, mem->device)) {
    GST_ERROR_OBJECT (allocator,
        "Could not create processor input view, hr: 0x%x", (guint) hr);
    goto done;
  }

succeeded:
  ret = TRUE;

done:
  GST_D3D11_MEMORY_UNLOCK (mem);

  return ret;
}

/**
 * gst_d3d11_memory_get_processor_output_view:
 * @mem: a #GstD3D11Memory
 * @video_device: a #ID3D11VideoDevice
 * @enumerator: a #ID3D11VideoProcessorEnumerator
 *
 * Returns: (transfer none) (nullable): a pointer to the
 * ID3D11VideoProcessorOutputView or %NULL if ID3D11VideoProcessorOutputView is
 * unavailable
 *
 * Since: 1.20
 */
ID3D11VideoProcessorOutputView *
gst_d3d11_memory_get_processor_output_view (GstD3D11Memory * mem,
    ID3D11VideoDevice * video_device,
    ID3D11VideoProcessorEnumerator * enumerator)
{
  g_return_val_if_fail (gst_is_d3d11_memory (GST_MEMORY_CAST (mem)), NULL);
  g_return_val_if_fail (video_device != NULL, NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);

  if (!gst_d3d11_memory_ensure_processor_output_view (mem, video_device,
          enumerator))
    return NULL;

  return mem->priv->processor_output_view;
}
