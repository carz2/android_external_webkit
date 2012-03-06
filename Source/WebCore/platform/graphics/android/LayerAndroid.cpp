#include "config.h"
#include "LayerAndroid.h"

#if USE(ACCELERATED_COMPOSITING)

#include "AndroidAnimation.h"
#include "ClassTracker.h"
#include "DrawExtra.h"
#include "DumpLayer.h"
#include "GLUtils.h"
#include "ImagesManager.h"
#include "InspectorCanvas.h"
#include "LayerGroup.h"
#include "MediaLayer.h"
#include "ParseCanvas.h"
#include "SkBitmapRef.h"
#include "SkDrawFilter.h"
#include "SkPaint.h"
#include "SkPicture.h"
#include "TilesManager.h"

#include <wtf/CurrentTime.h>
#include <math.h>

#define LAYER_DEBUG // Add diagonals for debugging
#undef LAYER_DEBUG

#include <cutils/log.h>
#include <wtf/text/CString.h>
#define XLOGC(...) android_printLog(ANDROID_LOG_DEBUG, "LayerAndroid", __VA_ARGS__)

#ifdef DEBUG

#undef XLOG
#define XLOG(...) android_printLog(ANDROID_LOG_DEBUG, "LayerAndroid", __VA_ARGS__)

#else

#undef XLOG
#define XLOG(...)

#endif // DEBUG

namespace WebCore {

static int gUniqueId;

class OpacityDrawFilter : public SkDrawFilter {
public:
    OpacityDrawFilter(int opacity) : m_opacity(opacity) { }
    virtual void filter(SkPaint* paint, Type)
    {
        paint->setAlpha(m_opacity);
    }
private:
    int m_opacity;
};

///////////////////////////////////////////////////////////////////////////////

LayerAndroid::LayerAndroid(RenderLayer* owner, SubclassType subclassType) : Layer(),
    m_haveClip(false),
    m_isIframe(false),
    m_backfaceVisibility(true),
    m_visible(true),
    m_preserves3D(false),
    m_anchorPointZ(0),
    m_recordingPicture(0),
    m_zValue(0),
    m_uniqueId(++gUniqueId),
    m_imageCRC(0),
    m_pictureUsed(0),
    m_scale(1),
    m_lastComputeTextureSize(0),
    m_owningLayer(owner),
    m_type(LayerAndroid::WebCoreLayer),
    m_subclassType(subclassType),
    m_hasText(true),
    m_layerGroup(0)
{
    m_backgroundColor = 0;

    m_preserves3D = false;
    m_dirtyRegion.setEmpty();
#ifdef DEBUG_COUNT
    ClassTracker::instance()->increment("LayerAndroid");
    ClassTracker::instance()->add(this);
#endif
}

LayerAndroid::LayerAndroid(const LayerAndroid& layer, SubclassType subclassType) : Layer(layer),
    m_haveClip(layer.m_haveClip),
    m_isIframe(layer.m_isIframe),
    m_zValue(layer.m_zValue),
    m_uniqueId(layer.m_uniqueId),
    m_owningLayer(layer.m_owningLayer),
    m_type(LayerAndroid::UILayer),
    m_hasText(true),
    m_layerGroup(0)
{
    m_imageCRC = layer.m_imageCRC;
    if (m_imageCRC)
        ImagesManager::instance()->retainImage(m_imageCRC);

    m_transform = layer.m_transform;
    m_backfaceVisibility = layer.m_backfaceVisibility;
    m_visible = layer.m_visible;
    m_backgroundColor = layer.m_backgroundColor;

    if (subclassType == LayerAndroid::CopyLayer)
        m_subclassType = layer.m_subclassType;
    else
        m_subclassType = subclassType;

    m_iframeOffset = layer.m_iframeOffset;
    m_offset = layer.m_offset;
    m_iframeScrollOffset = layer.m_iframeScrollOffset;
    m_recordingPicture = layer.m_recordingPicture;
    SkSafeRef(m_recordingPicture);

    m_preserves3D = layer.m_preserves3D;
    m_anchorPointZ = layer.m_anchorPointZ;
    m_drawTransform = layer.m_drawTransform;
    m_childrenTransform = layer.m_childrenTransform;
    m_pictureUsed = layer.m_pictureUsed;
    m_dirtyRegion = layer.m_dirtyRegion;
    m_scale = layer.m_scale;
    m_lastComputeTextureSize = 0;

    for (int i = 0; i < layer.countChildren(); i++)
        addChild(layer.getChild(i)->copy())->unref();

    KeyframesMap::const_iterator end = layer.m_animations.end();
    for (KeyframesMap::const_iterator it = layer.m_animations.begin(); it != end; ++it) {
        m_animations.add(it->first, it->second);
    }

    m_hasText = layer.m_hasText;

#ifdef DEBUG_COUNT
    ClassTracker::instance()->increment("LayerAndroid - recopy (UI)");
    ClassTracker::instance()->add(this);
#endif
}

void LayerAndroid::checkForPictureOptimizations()
{
    if (m_recordingPicture) {
        // Let's check if we have text or not. If we don't, we can limit
        // ourselves to scale 1!
        InspectorBounder inspectorBounder;
        InspectorCanvas checker(&inspectorBounder, m_recordingPicture);
        SkBitmap bitmap;
        bitmap.setConfig(SkBitmap::kARGB_8888_Config,
                         m_recordingPicture->width(),
                         m_recordingPicture->height());
        checker.setBitmapDevice(bitmap);
        checker.drawPicture(*m_recordingPicture);
        m_hasText = checker.hasText();
        if (!checker.hasContent()) {
            // no content to draw, discard picture so UI / tile generation
            // doesn't bother with it
            SkSafeUnref(m_recordingPicture);
            m_recordingPicture = 0;
        }
    }
}

LayerAndroid::LayerAndroid(SkPicture* picture) : Layer(),
    m_haveClip(false),
    m_isIframe(false),
    m_recordingPicture(picture),
    m_zValue(0),
    m_uniqueId(++gUniqueId),
    m_imageCRC(0),
    m_scale(1),
    m_lastComputeTextureSize(0),
    m_owningLayer(0),
    m_type(LayerAndroid::NavCacheLayer),
    m_subclassType(LayerAndroid::StandardLayer),
    m_hasText(true),
    m_layerGroup(0)
{
    m_backgroundColor = 0;
    SkSafeRef(m_recordingPicture);
    m_dirtyRegion.setEmpty();
#ifdef DEBUG_COUNT
    ClassTracker::instance()->increment("LayerAndroid - from picture");
    ClassTracker::instance()->add(this);
#endif
}

LayerAndroid::~LayerAndroid()
{
    if (m_imageCRC)
        ImagesManager::instance()->releaseImage(m_imageCRC);

    SkSafeUnref(m_recordingPicture);
    // Don't unref m_layerGroup, owned by BaseLayerAndroid
    m_animations.clear();
#ifdef DEBUG_COUNT
    ClassTracker::instance()->remove(this);
    if (m_type == LayerAndroid::WebCoreLayer)
        ClassTracker::instance()->decrement("LayerAndroid");
    else if (m_type == LayerAndroid::UILayer)
        ClassTracker::instance()->decrement("LayerAndroid - recopy (UI)");
    else if (m_type == LayerAndroid::NavCacheLayer)
        ClassTracker::instance()->decrement("LayerAndroid - from picture");
#endif
}

static int gDebugNbAnims = 0;

bool LayerAndroid::evaluateAnimations()
{
    double time = WTF::currentTime();
    gDebugNbAnims = 0;
    return evaluateAnimations(time);
}

bool LayerAndroid::hasAnimations() const
{
    for (int i = 0; i < countChildren(); i++) {
        if (getChild(i)->hasAnimations())
            return true;
    }
    return !!m_animations.size();
}

bool LayerAndroid::evaluateAnimations(double time)
{
    bool hasRunningAnimations = false;
    for (int i = 0; i < countChildren(); i++) {
        if (getChild(i)->evaluateAnimations(time))
            hasRunningAnimations = true;
    }

    m_hasRunningAnimations = false;
    int nbAnims = 0;
    KeyframesMap::const_iterator end = m_animations.end();
    for (KeyframesMap::const_iterator it = m_animations.begin(); it != end; ++it) {
        gDebugNbAnims++;
        nbAnims++;
        LayerAndroid* currentLayer = const_cast<LayerAndroid*>(this);
        m_hasRunningAnimations |= (it->second)->evaluate(currentLayer, time);
    }

    return hasRunningAnimations || m_hasRunningAnimations;
}

void LayerAndroid::initAnimations() {
    // tell auto-initializing animations to start now
    for (int i = 0; i < countChildren(); i++)
        getChild(i)->initAnimations();

    KeyframesMap::const_iterator localBegin = m_animations.begin();
    KeyframesMap::const_iterator localEnd = m_animations.end();
    for (KeyframesMap::const_iterator localIt = localBegin; localIt != localEnd; ++localIt)
        (localIt->second)->suggestBeginTime(WTF::currentTime());
}

void LayerAndroid::addDirtyArea()
{
    IntSize layerSize(getSize().width(), getSize().height());

    FloatRect area = TilesManager::instance()->shader()->rectInInvScreenCoord(m_drawTransform, layerSize);
    FloatRect clippingRect = TilesManager::instance()->shader()->rectInScreenCoord(m_clippingRect);
    FloatRect clip = TilesManager::instance()->shader()->convertScreenCoordToInvScreenCoord(clippingRect);

    area.intersect(clip);
    IntRect dirtyArea(area.x(), area.y(), area.width(), area.height());
    m_state->addDirtyArea(dirtyArea);
}

void LayerAndroid::addAnimation(PassRefPtr<AndroidAnimation> prpAnim)
{
    RefPtr<AndroidAnimation> anim = prpAnim;
    pair<String, int> key(anim->name(), anim->type());
    removeAnimationsForProperty(anim->type());
    m_animations.add(key, anim);
}

void LayerAndroid::removeAnimationsForProperty(AnimatedPropertyID property)
{
    KeyframesMap::const_iterator end = m_animations.end();
    Vector<pair<String, int> > toDelete;
    for (KeyframesMap::const_iterator it = m_animations.begin(); it != end; ++it) {
        if ((it->second)->type() == property)
            toDelete.append(it->first);
    }

    for (unsigned int i = 0; i < toDelete.size(); i++)
        m_animations.remove(toDelete[i]);
}

void LayerAndroid::removeAnimationsForKeyframes(const String& name)
{
    KeyframesMap::const_iterator end = m_animations.end();
    Vector<pair<String, int> > toDelete;
    for (KeyframesMap::const_iterator it = m_animations.begin(); it != end; ++it) {
        if ((it->second)->name() == name)
            toDelete.append(it->first);
    }

    for (unsigned int i = 0; i < toDelete.size(); i++)
        m_animations.remove(toDelete[i]);
}

// We only use the bounding rect of the layer as mask...
// FIXME: use a real mask?
void LayerAndroid::setMaskLayer(LayerAndroid* layer)
{
    if (layer)
        m_haveClip = true;
}

void LayerAndroid::setBackgroundColor(SkColor color)
{
    m_backgroundColor = color;
}

FloatPoint LayerAndroid::translation() const
{
    TransformationMatrix::DecomposedType tDecomp;
    m_transform.decompose(tDecomp);
    FloatPoint p(tDecomp.translateX, tDecomp.translateY);
    return p;
}

SkRect LayerAndroid::bounds() const
{
    SkRect rect;
    bounds(&rect);
    return rect;
}

void LayerAndroid::bounds(SkRect* rect) const
{
    const SkPoint& pos = this->getPosition();
    const SkSize& size = this->getSize();

    // The returned rect has the translation applied
    // FIXME: apply the full transform to the rect,
    // and fix the text selection accordingly
    FloatPoint p(pos.fX, pos.fY);
    p = m_transform.mapPoint(p);
    rect->fLeft = p.x();
    rect->fTop = p.y();
    rect->fRight = p.x() + size.width();
    rect->fBottom = p.y() + size.height();
}

static bool boundsIsUnique(const SkTDArray<SkRect>& region,
                           const SkRect& local)
{
    for (int i = 0; i < region.count(); i++) {
        if (region[i].contains(local))
            return false;
    }
    return true;
}

void LayerAndroid::clipArea(SkTDArray<SkRect>* region) const
{
    SkRect local;
    local.set(0, 0, std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max());
    clipInner(region, local);
}

void LayerAndroid::clipInner(SkTDArray<SkRect>* region,
                             const SkRect& local) const
{
    SkRect localBounds;
    bounds(&localBounds);
    localBounds.intersect(local);
    if (localBounds.isEmpty())
        return;
    if (m_recordingPicture && boundsIsUnique(*region, localBounds))
        *region->append() = localBounds;
    for (int i = 0; i < countChildren(); i++)
        getChild(i)->clipInner(region, m_haveClip ? localBounds : local);
}

LayerAndroid* LayerAndroid::updateFixedLayerPosition(SkRect viewport,
                                                     LayerAndroid* parentIframeLayer)
{
    LayerAndroid* iframe = parentIframeLayer;

    // If this is an iframe, accumulate the offset from the parent with
    // current position, and change the parent pointer.
    if (m_isIframe) {
        // If this is the top level, take the current position
        SkPoint parentOffset;
        parentOffset.set(0,0);
        if (iframe)
            parentOffset = iframe->getPosition();

        SkPoint offset = parentOffset + getPosition();
        m_iframeOffset = IntPoint(offset.fX, offset.fY);

        iframe = this;
    }

    return iframe;
}

void LayerAndroid::updateFixedLayersPositions(SkRect viewport, LayerAndroid* parentIframeLayer)
{
    XLOG("updating fixed positions, using viewport %fx%f - %fx%f",
         viewport.fLeft, viewport.fTop,
         viewport.width(), viewport.height());

    LayerAndroid* iframeLayer = updateFixedLayerPosition(viewport, parentIframeLayer);

    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->updateFixedLayersPositions(viewport, iframeLayer);
}

void LayerAndroid::updatePositions()
{
    // apply the viewport to us
    if (!isFixed()) {
        // turn our fields into a matrix.
        //
        // FIXME: this should happen in the caller, and we should remove these
        // fields from our subclass
        SkMatrix matrix;
        GLUtils::toSkMatrix(matrix, m_transform);
        this->setMatrix(matrix);
    }

    // now apply it to our children
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->updatePositions();
}

void LayerAndroid::updateGLPositionsAndScale(const TransformationMatrix& parentMatrix,
                                             const FloatRect& clipping, float opacity, float scale)
{
    m_atomicSync.lock();
    IntSize layerSize(getSize().width(), getSize().height());
    FloatPoint anchorPoint(getAnchorPoint().fX, getAnchorPoint().fY);
    FloatPoint position(getPosition().fX - m_offset.x(), getPosition().fY - m_offset.y());
    float originX = anchorPoint.x() * layerSize.width();
    float originY = anchorPoint.y() * layerSize.height();
    TransformationMatrix localMatrix;
    if (!isFixed())
        localMatrix = parentMatrix;
    localMatrix.translate3d(originX + position.x(),
                            originY + position.y(),
                            anchorPointZ());
    localMatrix.multiply(m_transform);
    localMatrix.translate3d(-originX,
                            -originY,
                            -anchorPointZ());

    m_atomicSync.unlock();
    setDrawTransform(localMatrix);
    if (m_drawTransform.isIdentityOrTranslation()) {
        // adjust the translation coordinates of the draw transform matrix so
        // that layers (defined in content coordinates) will align to display/view pixels
        float desiredContentX = round(m_drawTransform.m41() * scale) / scale;
        float desiredContentY = round(m_drawTransform.m42() * scale) / scale;
        XLOG("fudging translation from %f, %f to %f, %f",
             m_drawTransform.m41(), m_drawTransform.m42(),
             desiredContentX, desiredContentY);
        m_drawTransform.setM41(desiredContentX);
        m_drawTransform.setM42(desiredContentY);
    }

    m_zValue = TilesManager::instance()->shader()->zValue(m_drawTransform, getSize().width(), getSize().height());

    m_atomicSync.lock();
    m_scale = scale;
    m_atomicSync.unlock();

    opacity *= getOpacity();
    setDrawOpacity(opacity);

    if (m_haveClip) {
        // The clipping rect calculation and intersetion will be done in documents coordinates.
        FloatRect rect(0, 0, layerSize.width(), layerSize.height());
        FloatRect clip = m_drawTransform.mapRect(rect);
        clip.intersect(clipping);
        setDrawClip(clip);
    } else {
        setDrawClip(clipping);
    }

    if (!m_backfaceVisibility
         && m_drawTransform.inverse().m33() < 0) {
         setVisible(false);
         return;
    } else {
         setVisible(true);
    }

    int count = this->countChildren();
    if (!count)
        return;

    // Flatten to 2D if the layer doesn't preserve 3D.
    if (!preserves3D()) {
        localMatrix.setM13(0);
        localMatrix.setM23(0);
        localMatrix.setM31(0);
        localMatrix.setM32(0);
        localMatrix.setM33(1);
        localMatrix.setM34(0);
        localMatrix.setM43(0);
    }

    // now apply it to our children

    TransformationMatrix childMatrix;
    childMatrix = localMatrix;
    childMatrix.translate3d(m_offset.x(), m_offset.y(), 0);
    if (!m_childrenTransform.isIdentity()) {
        childMatrix.translate(getSize().width() * 0.5f, getSize().height() * 0.5f);
        childMatrix.multiply(m_childrenTransform);
        childMatrix.translate(-getSize().width() * 0.5f, -getSize().height() * 0.5f);
    }
    for (int i = 0; i < count; i++)
        this->getChild(i)->updateGLPositionsAndScale(childMatrix, drawClip(), opacity, scale);
}

bool LayerAndroid::visible() {
    // TODO: avoid climbing tree each access
    LayerAndroid* current = this;
    while (current->getParent()) {
        if (!current->m_visible)
            return false;
        current = static_cast<LayerAndroid*>(current->getParent());
    }
    return true;
}

void LayerAndroid::setContentsImage(SkBitmapRef* img)
{
    ImageTexture* image = ImagesManager::instance()->setImage(img);
    ImagesManager::instance()->releaseImage(m_imageCRC);
    m_imageCRC = image ? image->imageCRC() : 0;
}

bool LayerAndroid::needsTexture()
{
    return (m_recordingPicture
        && m_recordingPicture->width() && m_recordingPicture->height());
}

IntRect LayerAndroid::clippedRect() const
{
    IntRect r(0, 0, getWidth(), getHeight());
    IntRect tr = m_drawTransform.mapRect(r);
    IntRect cr = TilesManager::instance()->shader()->clippedRectWithViewport(tr);
    IntRect rect = m_drawTransform.inverse().mapRect(cr);
    return rect;
}

int LayerAndroid::nbLayers()
{
    int nb = 0;
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        nb += this->getChild(i)->nbLayers();
    return nb+1;
}

int LayerAndroid::nbTexturedLayers()
{
    int nb = 0;
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        nb += this->getChild(i)->nbTexturedLayers();
    if (needsTexture())
        nb++;
    return nb;
}

void LayerAndroid::showLayer(int indent)
{
    char spaces[256];
    memset(spaces, 0, 256);
    for (int i = 0; i < indent; i++)
        spaces[i] = ' ';

    if (!indent) {
        XLOGC("\n\n--- LAYERS TREE ---");
        IntRect documentViewport(TilesManager::instance()->shader()->documentViewport());
        XLOGC("documentViewport(%d, %d, %d, %d)",
              documentViewport.x(), documentViewport.y(),
              documentViewport.width(), documentViewport.height());
    }

    IntRect r(0, 0, getWidth(), getHeight());
    IntRect tr = m_drawTransform.mapRect(r);
    IntRect visible = visibleArea();
    IntRect clip(m_clippingRect.x(), m_clippingRect.y(),
                 m_clippingRect.width(), m_clippingRect.height());
    XLOGC("%s %s (%d) [%d:0x%x] - %s %s - area (%d, %d, %d, %d) - visible (%d, %d, %d, %d) "
          "clip (%d, %d, %d, %d) %s %s prepareContext(%x), pic w: %d h: %d",
          spaces, subclassName().latin1().data(), m_subclassType, uniqueId(), m_owningLayer,
          needsTexture() ? "needs a texture" : "no texture",
          m_imageCRC ? "has an image" : "no image",
          tr.x(), tr.y(), tr.width(), tr.height(),
          visible.x(), visible.y(), visible.width(), visible.height(),
          clip.x(), clip.y(), clip.width(), clip.height(),
          contentIsScrollable() ? "SCROLLABLE" : "",
          isFixed() ? "FIXED" : "",
          m_recordingPicture,
          m_recordingPicture ? m_recordingPicture->width() : -1,
          m_recordingPicture ? m_recordingPicture->height() : -1);

    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->showLayer(indent + 1);
}

void LayerAndroid::setIsPainting(Layer* drawingTree)
{
    XLOG("setting layer %p as painting, needs texture %d, drawing tree %p",
         this, needsTexture(), drawingTree);
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->setIsPainting(drawingTree);


    LayerAndroid* drawingLayer = 0;
    if (drawingTree)
        drawingLayer = static_cast<LayerAndroid*>(drawingTree)->findById(uniqueId());

    obtainTextureForPainting(drawingLayer);
}

void LayerAndroid::mergeInvalsInto(LayerAndroid* replacementTree)
{
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->mergeInvalsInto(replacementTree);

    LayerAndroid* replacementLayer = replacementTree->findById(uniqueId());
    if (replacementLayer)
        replacementLayer->markAsDirty(m_dirtyRegion);
}

bool LayerAndroid::updateWithTree(LayerAndroid* newTree)
{
// Disable fast update for now
#if (0)
    bool needsRepaint = false;
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        needsRepaint |= this->getChild(i)->updateWithTree(newTree);

    if (newTree) {
        LayerAndroid* newLayer = newTree->findById(uniqueId());
        needsRepaint |= updateWithLayer(newLayer);
    }
    return needsRepaint;
#else
    return true;
#endif
}

// Return true to indicate to WebViewCore that the updates
// are too complicated to be fully handled and we need a full
// call to webkit (e.g. handle repaints)
bool LayerAndroid::updateWithLayer(LayerAndroid* layer)
{
    if (!layer)
        return true;

    android::AutoMutex lock(m_atomicSync);
    m_position = layer->m_position;
    m_anchorPoint = layer->m_anchorPoint;
    m_size = layer->m_size;
    m_opacity = layer->m_opacity;
    m_transform = layer->m_transform;

    if (m_imageCRC != layer->m_imageCRC)
        m_visible = false;

    if ((m_recordingPicture != layer->m_recordingPicture)
        || (m_imageCRC != layer->m_imageCRC))
        return true;

    return false;
}

void LayerAndroid::obtainTextureForPainting(LayerAndroid* drawingLayer)
{
    if (!needsTexture())
        return;

    // layer group init'd with previous drawing layer
    m_layerGroup->initializeGroup(this, m_dirtyRegion, drawingLayer);
    m_dirtyRegion.setEmpty();
}


static inline bool compareLayerZ(const LayerAndroid* a, const LayerAndroid* b)
{
    return a->zValue() > b->zValue();
}

void LayerAndroid::assignGroups(Vector<LayerGroup*>* allGroups)
{
    // recurse through layers in draw order
    // if a layer needs isolation (e.g. has animation, is fixed, overflow:scroll)
    //     create new layer group on the stack

    bool needsIsolation = false;
    LayerGroup* currentLayerGroup = 0;
    if (!allGroups->isEmpty())
        currentLayerGroup = allGroups->at(0);

    // TODO: compare layer with group on top of stack - fixed? overscroll? transformed?
    needsIsolation = isFixed() || (m_animations.size() != 0);

    if (!currentLayerGroup || needsIsolation || true) {
        currentLayerGroup = new LayerGroup();
        allGroups->append(currentLayerGroup);
    }

    currentLayerGroup->addLayer(this);
    m_layerGroup = currentLayerGroup;

    // pass the layergroup through children in drawing order, so that they may
    // attach themselves (and paint on it) if possible, or ignore it and create
    // a new one if not
    int count = this->countChildren();
    if (count > 0) {
        Vector <LayerAndroid*> sublayers;
        for (int i = 0; i < count; i++)
            sublayers.append(getChild(i));

        // sort for the transparency
        std::stable_sort(sublayers.begin(), sublayers.end(), compareLayerZ);
        for (int i = 0; i < count; i++)
            sublayers[i]->assignGroups(allGroups);
    }
}

// We call this in WebViewCore, when copying the tree of layers.
// As we construct a new tree that will be passed on the UI,
// we mark the webkit-side tree as having no more dirty region
// (otherwise we would continuously have those dirty region UI-side)
void LayerAndroid::clearDirtyRegion()
{
    int count = this->countChildren();
    for (int i = 0; i < count; i++)
        this->getChild(i)->clearDirtyRegion();

    m_dirtyRegion.setEmpty();
}

IntRect LayerAndroid::unclippedArea()
{
    IntRect area;
    area.setX(0);
    area.setY(0);
    area.setWidth(getSize().width());
    area.setHeight(getSize().height());
    return area;
}

IntRect LayerAndroid::visibleArea()
{
    IntRect area = unclippedArea();
    // First, we get the transformed area of the layer,
    // in document coordinates
    IntRect rect = m_drawTransform.mapRect(area);
    int dx = rect.x();
    int dy = rect.y();

    // Then we apply the clipping
    IntRect clip(m_clippingRect);
    rect.intersect(clip);

    // Now clip with the viewport in documents coordinate
    IntRect documentViewport(TilesManager::instance()->shader()->documentViewport());
    rect.intersect(documentViewport);

    // Finally, let's return the visible area, in layers coordinate
    rect.move(-dx, -dy);
    return rect;
}

bool LayerAndroid::drawCanvas(SkCanvas* canvas)
{
    if (!m_visible)
        return false;

    bool askScreenUpdate = false;

    {
        SkAutoCanvasRestore acr(canvas, true);
        SkRect r;
        r.set(m_clippingRect.x(), m_clippingRect.y(),
              m_clippingRect.x() + m_clippingRect.width(),
              m_clippingRect.y() + m_clippingRect.height());
        canvas->clipRect(r);
        SkMatrix matrix;
        GLUtils::toSkMatrix(matrix, m_drawTransform);
        SkMatrix canvasMatrix = canvas->getTotalMatrix();
        matrix.postConcat(canvasMatrix);
        canvas->setMatrix(matrix);
        onDraw(canvas, m_drawOpacity, 0);
    }

    // When the layer is dirty, the UI thread should be notified to redraw.
    askScreenUpdate |= drawChildrenCanvas(canvas);
    m_atomicSync.lock();
    if (askScreenUpdate || m_hasRunningAnimations || m_drawTransform.hasPerspective())
        addDirtyArea();

    m_atomicSync.unlock();
    return askScreenUpdate;
}

bool LayerAndroid::drawGL(bool layerTilesDisabled)
{
    if (!layerTilesDisabled && m_imageCRC) {
        ImageTexture* imageTexture = ImagesManager::instance()->retainImage(m_imageCRC);
        if (imageTexture)
            imageTexture->drawGL(this, getOpacity());
        ImagesManager::instance()->releaseImage(m_imageCRC);
    }

    m_state->glExtras()->drawGL(this);
    bool askScreenUpdate = false;

    m_atomicSync.lock();
    if (m_hasRunningAnimations || m_drawTransform.hasPerspective()) {
        askScreenUpdate = true;
        addDirtyArea();
    }

    m_atomicSync.unlock();
    return askScreenUpdate;
}

bool LayerAndroid::drawChildrenCanvas(SkCanvas* canvas)
{
    bool askScreenUpdate = false;
    int count = this->countChildren();
    if (count > 0) {
        Vector <LayerAndroid*> sublayers;
        for (int i = 0; i < count; i++)
            sublayers.append(this->getChild(i));

        // now we sort for the transparency
        std::stable_sort(sublayers.begin(), sublayers.end(), compareLayerZ);
        for (int i = 0; i < count; i++) {
            LayerAndroid* layer = sublayers[i];
            askScreenUpdate |= layer->drawCanvas(canvas);
        }
    }

    return askScreenUpdate;
}

void LayerAndroid::contentDraw(SkCanvas* canvas)
{
    if (m_recordingPicture)
      canvas->drawPicture(*m_recordingPicture);

    if (TilesManager::instance()->getShowVisualIndicator()) {
        float w = getSize().width();
        float h = getSize().height();
        SkPaint paint;
        paint.setARGB(128, 255, 0, 0);
        canvas->drawLine(0, 0, w, h, paint);
        canvas->drawLine(0, h, w, 0, paint);
        paint.setARGB(128, 0, 255, 0);
        canvas->drawLine(0, 0, 0, h, paint);
        canvas->drawLine(0, h, w, h, paint);
        canvas->drawLine(w, h, w, 0, paint);
        canvas->drawLine(w, 0, 0, 0, paint);
    }
}

void LayerAndroid::onDraw(SkCanvas* canvas, SkScalar opacity, android::DrawExtra* extra)
{
    if (m_haveClip) {
        SkRect r;
        r.set(0, 0, getSize().width(), getSize().height());
        canvas->clipRect(r);
        return;
    }

    if (!prepareContext())
        return;

    // we just have this save/restore for opacity...
    SkAutoCanvasRestore restore(canvas, true);

    int canvasOpacity = SkScalarRound(opacity * 255);
    if (canvasOpacity < 255)
        canvas->setDrawFilter(new OpacityDrawFilter(canvasOpacity));

    if (m_imageCRC) {
        ImageTexture* imageTexture = ImagesManager::instance()->retainImage(m_imageCRC);
        m_dirtyRegion.setEmpty();
        if (imageTexture) {
            SkRect dest;
            dest.set(0, 0, getSize().width(), getSize().height());
            imageTexture->drawCanvas(canvas, dest);
        }
        ImagesManager::instance()->releaseImage(m_imageCRC);
    }
    contentDraw(canvas);
    if (extra)
        extra->draw(canvas, this);
}

SkPicture* LayerAndroid::recordContext()
{
    if (prepareContext(true))
        return m_recordingPicture;
    return 0;
}

bool LayerAndroid::prepareContext(bool force)
{
    if (masksToBounds())
        return false;

    if (force || !m_recordingPicture ||
        (m_recordingPicture &&
         ((m_recordingPicture->width() != (int) getSize().width()) ||
          (m_recordingPicture->height() != (int) getSize().height())))) {
        SkSafeUnref(m_recordingPicture);
        m_recordingPicture = new SkPicture();
    }

    return m_recordingPicture;
}

SkRect LayerAndroid::subtractLayers(const SkRect& visibleRect) const
{
    SkRect result;
    if (m_recordingPicture) {
        // FIXME: This seems wrong. localToGlobal() applies the full local transform,
        // se surely we should operate globalMatrix on size(), not bounds() with
        // the position removed? Perhaps we never noticed the bug because most
        // layers don't use a local transform?
        // See http://b/5338388
        SkRect globalRect = bounds();
        globalRect.offset(-getPosition()); // localToGlobal adds in position
        SkMatrix globalMatrix;
        localToGlobal(&globalMatrix);
        globalMatrix.mapRect(&globalRect);
        SkIRect roundedGlobal;
        globalRect.round(&roundedGlobal);
        SkIRect iVisibleRect;
        visibleRect.round(&iVisibleRect);
        SkRegion visRegion(iVisibleRect);
        visRegion.op(roundedGlobal, SkRegion::kDifference_Op);
        result.set(visRegion.getBounds());
#if DEBUG_NAV_UI
        SkDebugf("%s visibleRect=(%g,%g,r=%g,b=%g) globalRect=(%g,%g,r=%g,b=%g)"
            "result=(%g,%g,r=%g,b=%g)", __FUNCTION__,
            visibleRect.fLeft, visibleRect.fTop,
            visibleRect.fRight, visibleRect.fBottom,
            globalRect.fLeft, globalRect.fTop,
            globalRect.fRight, globalRect.fBottom,
            result.fLeft, result.fTop, result.fRight, result.fBottom);
#endif
    } else
        result = visibleRect;
    for (int i = 0; i < countChildren(); i++)
        result = getChild(i)->subtractLayers(result);
    return result;
}

void LayerAndroid::dumpLayer(FILE* file, int indentLevel) const
{
    writeHexVal(file, indentLevel + 1, "layer", (int)this);
    writeIntVal(file, indentLevel + 1, "layerId", m_uniqueId);
    writeIntVal(file, indentLevel + 1, "haveClip", m_haveClip);
    writeIntVal(file, indentLevel + 1, "isFixed", isFixed());
    writeIntVal(file, indentLevel + 1, "m_isIframe", m_isIframe);
    writeIntPoint(file, indentLevel + 1, "m_iframeOffset", m_iframeOffset);

    writeFloatVal(file, indentLevel + 1, "opacity", getOpacity());
    writeSize(file, indentLevel + 1, "size", getSize());
    writePoint(file, indentLevel + 1, "position", getPosition());
    writePoint(file, indentLevel + 1, "anchor", getAnchorPoint());

    writeMatrix(file, indentLevel + 1, "drawMatrix", m_drawTransform);
    writeMatrix(file, indentLevel + 1, "transformMatrix", m_transform);
    writeRect(file, indentLevel + 1, "clippingRect", SkRect(m_clippingRect));

    if (m_recordingPicture) {
        writeIntVal(file, indentLevel + 1, "m_recordingPicture.width", m_recordingPicture->width());
        writeIntVal(file, indentLevel + 1, "m_recordingPicture.height", m_recordingPicture->height());
    }
}

void LayerAndroid::dumpLayers(FILE* file, int indentLevel) const
{
    writeln(file, indentLevel, "{");

    dumpLayer(file, indentLevel);

    if (countChildren()) {
        writeln(file, indentLevel + 1, "children = [");
        for (int i = 0; i < countChildren(); i++) {
            if (i > 0)
                writeln(file, indentLevel + 1, ", ");
            getChild(i)->dumpLayers(file, indentLevel + 1);
        }
        writeln(file, indentLevel + 1, "];");
    }
    writeln(file, indentLevel, "}");
}

void LayerAndroid::dumpToLog() const
{
    FILE* file = fopen("/data/data/com.android.browser/layertmp", "w");
    dumpLayers(file, 0);
    fclose(file);
    file = fopen("/data/data/com.android.browser/layertmp", "r");
    char buffer[512];
    bzero(buffer, sizeof(buffer));
    while (fgets(buffer, sizeof(buffer), file))
        SkDebugf("%s", buffer);
    fclose(file);
}

LayerAndroid* LayerAndroid::findById(int match)
{
    if (m_uniqueId == match)
        return this;
    for (int i = 0; i < countChildren(); i++) {
        LayerAndroid* result = getChild(i)->findById(match);
        if (result)
            return result;
    }
    return 0;
}

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING)
