#include "Global.h"

#include <errno.h>

#include "MapView.h"
#include "MainWindow.h"
#include "PropertiesDock.h"
#include "IDocument.h"
#include "ILayer.h"
#include "LayerIterator.h"
#include "ImageMapLayer.h"
#include "IMapAdapter.h"
#include "IMapWatermark.h"
#include "Feature.h"
#include "Interaction.h"
#include "IPaintStyle.h"
#include "Projection.h"
#include "qgps.h"
#include "qgpsdevice.h"

#include "MapRenderer.h"

#ifdef USE_WEBKIT
    #include "browserimagemanager.h"
#endif
#include "MerkaartorPreferences.h"
#include "SvgCache.h"

#include <QTime>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QToolTip>
#include <QMap>
#include <QSet>
#include <QtConcurrentMap>

// from wikipedia
#define EQUATORIALRADIUS 6378137.0
#define LAT_ANG_PER_M 1.0 / EQUATORIALRADIUS
#define TEST_RFLAGS(x) p->ROptions.options.testFlag(x)
#define TILE_SIZE 256

inline uint qHash(const QPoint& p)
{
    return (uint)(p.y() + (p.x() << 16));
}

QCache<TILE_TYPE, QImage> tileCache;
QList<TILE_TYPE> tiles;
QReadWriteLock tileLock;
#define TILE_CONSTRUCTOR(x, y) QPoint(x, y)
#define TILE_X(t) t.x()
#define TILE_Y(t) t.y()

class MapViewPrivate
{
public:
    QTransform theTransform;
    QTransform theInvertedTransform;
    qreal PixelPerM;
    int ZoomLevel;
    CoordBox Viewport;
    QPoint theVectorPanDelta;
    qreal theVectorRotation;
    QMap<RenderPriority, QSet <Feature*> > theFeatures;
    QList<Node*> theVirtualNodes;
    MapRenderer renderer;
    RendererOptions ROptions;

    Projection theProjection;
    IDocument* theDocument;
    Interaction* theInteraction;

    bool BackgroundOnlyPanZoom;
    QTransform BackgroundOnlyVpTransform;

    qreal tileSizeCoordW;
    qreal tileSizeCoordH;
    QPointF tileOriginCoord;
    QRect tileViewport;

    QLabel *TL, *TR, *BL, *BR;

    QFuture<void> renderGathering;

    MapViewPrivate()
      : PixelPerM(0.0), Viewport(WORLD_COORDBOX), theVectorRotation(0.0)
      , BackgroundOnlyPanZoom(false)
      , theDocument(0)
      , theInteraction(0)
    {}

    QList<TILE_TYPE> tileCalc(const QRect& rect)
    {
//        qDebug() << "mapview transform: " << theTransform;

        QPointF tl = theInvertedTransform.map(QPointF(rect.topLeft()));
        QPointF br = theInvertedTransform.map(QPointF(rect.bottomRight()));
        QRectF projRect(tl, br);

        //    qDebug() << "tileSizeCoord" << p->tileSizeCoord;
        //    qDebug() << "  " << screen.width() << screen.height();

        tileViewport.setLeft(((projRect.left()-tileOriginCoord.x()) / tileSizeCoordW) - 1);
        tileViewport.setTop(((projRect.top()-tileOriginCoord.y()) / tileSizeCoordH) - 1);
        tileViewport.setRight(((projRect.right()-tileOriginCoord.x()) / tileSizeCoordW) + 1);
        tileViewport.setBottom(((projRect.bottom()-tileOriginCoord.y()) / tileSizeCoordH) + 1);

        if (renderGathering.isRunning()) {
            renderGathering.cancel();
            renderGathering.waitForFinished();
        }
        tiles.clear();
        for (int i=tileViewport.top(); i<=tileViewport.bottom(); ++i)
            for (int j=tileViewport.left(); j<=tileViewport.right(); ++j) {
                TILE_TYPE tile = TILE_CONSTRUCTOR(j, i);
                tiles << tile;
            }

        //    qDebug() << "  " << p->tileViewport;
        //    qDebug() << "  " << viewport.topLeft() << viewport.bottomRight();
        //    qDebug() << "  " << l*p->tileSizeCoord - 180. << 90. - t*p->tileSizeCoord << r*p->tileSizeCoord -180. << 90. - b*p->tileSizeCoord;

        return tiles;
    }


    void resetTileCache(const QRect& rect)
    {
        tileLock.lockForWrite();
        tileCache.clear();
        tileLock.unlock();
        tileOriginCoord = theInvertedTransform.map(QPointF(rect.topLeft()));
        if (theDocument) {
            QPointF tl = theInvertedTransform.map(QPointF(rect.topLeft()));
            QPointF br = theInvertedTransform.map(QPointF(rect.bottomRight())+QPointF(1,1));
            QRectF projRect(tl, br);
            tileSizeCoordW = (projRect.width()) / rect.width() * TILE_SIZE;
//            tileSizeCoordH = (projRect.height()) / rect.height() * TILE_SIZE;
            tileSizeCoordH = tileSizeCoordW * projRect.height() / fabs(projRect.height());
        }

    }

//    void renderTile(MapView* v, TILE_TYPE tile)
//    {
//        if (!tileCache.contains(tile)) {
//            QMap<RenderPriority, QSet <Feature*> > theFeatures;

//            QPointF projTL((tile.x()*tileSizeCoordW)+tileOriginCoord.x(), (tile.y()*tileSizeCoordH)+tileOriginCoord.y());
//            QPointF projBR(((tile.x()+1)*tileSizeCoordW)+tileOriginCoord.x(), ((tile.y()+1)*tileSizeCoordH)+tileOriginCoord.y());
//            QRectF projR(projTL, projBR);

//            qreal z=1.0625;
//            qreal dlat = (projR.top()-projR.bottom())*(z-1)/2;
//            qreal dlon = (projR.right()-projR.left())*(z-1)/2;
//            projR.setBottom(projR.bottom()-dlat);
//            projR.setLeft(projR.left()-dlon);
//            projR.setTop(projR.top()+dlat);
//            projR.setRight(projR.right()+dlon);

//            Coord tl = theProjection.inverse2Coord(projR.topLeft());
//            Coord br = theProjection.inverse2Coord(projR.bottomRight());
//            CoordBox invalidRect(tl, br);
////            qDebug() << invalidRect;

//            for (int i=0; i<theDocument->layerSize(); ++i)
//                g_backend.getFeatureSet(theDocument->getLayer(i), theFeatures, invalidRect, theProjection);

//            QImage* img = new QImage(TILE_SIZE, TILE_SIZE, QImage::Format_ARGB32);
//            img->fill(Qt::transparent);

//            QPainter P(img);
//            if (M_PREFS->getUseAntiAlias())
//                P.setRenderHint(QPainter::Antialiasing);
//            renderer.render(&P, theFeatures, projR, /*QRect(0, 0, TILE_SIZE, TILE_SIZE)*/QRect(-8, -8, TILE_SIZE+16, TILE_SIZE+16), PixelPerM, ROptions);
//            P.end();
//            tileCache.insert(tile, img);
////            if (theFeatures.size())
////                img->save(QString("c:/temp/%1-%2.png").arg(tile.x()).arg(tile.y()));
//        }
//    }

};


/****************/

struct RenderTile
{
    RenderTile(MapView* vw)
        : p(vw->p) { }

    typedef void result_type;

    void operator()(const TILE_TYPE& tile)
    {
#if defined(__MINGW32__)
        // Workaround for QTBUG-19886
        asm volatile ("mov %esp, %eax");
        asm volatile ("and $0xf, %eax");
        asm volatile ("je alignmentok");
        asm volatile ("push %eax");
        asm volatile ("push %eax");
        asm volatile ("alignmentok:");
#endif

        tileLock.lockForRead();
        if (!tileCache.contains(tile)) {
            tileLock.unlock();

//            qDebug() << "render tile: " << tile << TILE_X(tile) << TILE_Y(tile);
            QMap<RenderPriority, QSet <Feature*> > theFeatures;

            QPointF projTL((TILE_X(tile)*p->tileSizeCoordW)+p->tileOriginCoord.x(), (TILE_Y(tile)*p->tileSizeCoordH)+p->tileOriginCoord.y());
            QPointF projBR(((TILE_X(tile)+1)*p->tileSizeCoordW)+p->tileOriginCoord.x(), ((TILE_Y(tile)+1)*p->tileSizeCoordH)+p->tileOriginCoord.y());
            QRectF projR(projTL, projBR);

#define TILE_SURROUND 2.0
            qreal z = TILE_SURROUND * ((TILE_SIZE*TILE_SURROUND) / (p->theTransform.m11()*projR.width()*TILE_SURROUND));    // Adjust to main transform
            qreal dlat = (projR.top()-projR.bottom())*(z-1)/2;
            qreal dlon = (projR.right()-projR.left())*(z-1)/2;
            projR.setBottom(projR.bottom()-dlat);
            projR.setLeft(projR.left()-dlon);
            projR.setTop(projR.top()+dlat);
            projR.setRight(projR.right()+dlon);

            Coord tl = p->theProjection.inverse2Coord(projR.topLeft());
            Coord br = p->theProjection.inverse2Coord(projR.bottomRight());
            CoordBox invalidRect(tl, br);

            for (int i=0; i<p->theDocument->layerSize(); ++i)
                g_backend.getFeatureSet(p->theDocument->getLayer(i), theFeatures, invalidRect, p->theProjection);

            QImage* img = new QImage(TILE_SIZE, TILE_SIZE, QImage::Format_ARGB32);
            img->fill(Qt::transparent);

            QPainter P(img);
            if (M_PREFS->getUseAntiAlias())
                P.setRenderHint(QPainter::Antialiasing);
            MapRenderer r;
            r.render(&P, theFeatures, projR, /*QRect(0, 0, TILE_SIZE, TILE_SIZE)*/QRect(-((TILE_SIZE*TILE_SURROUND)-TILE_SIZE)/2, -((TILE_SIZE*TILE_SURROUND)-TILE_SIZE)/2, TILE_SIZE*TILE_SURROUND, TILE_SIZE*TILE_SURROUND), p->PixelPerM, p->ROptions);
            P.end();
            tileLock.lockForWrite();
            tileCache.insert(tile, img);
//            if (theFeatures.size())
//                img->save(QString("c:/temp/%1-%2.png").arg(tile.x()).arg(tile.y()));
        }
        tileLock.unlock();
    }

    MapViewPrivate* p;
};

/*********************/

MapView::MapView(QWidget* parent) :
    QWidget(parent), Main(dynamic_cast<MainWindow*>(parent)), StaticBackground(0), StaticBuffer(0),
        SelectionLocked(false),lockIcon(0),
        p(new MapViewPrivate)
{
    installEventFilter(Main);

    setMouseTracking(true);
    setAttribute(Qt::WA_NoSystemBackground);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true);

    MoveRightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(MoveRightShortcut, SIGNAL(activated()), this, SLOT(on_MoveRight_activated()));
    MoveLeftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(MoveLeftShortcut, SIGNAL(activated()), this, SLOT(on_MoveLeft_activated()));
    MoveUpShortcut = new QShortcut(QKeySequence(Qt::Key_Up), this);
    connect(MoveUpShortcut, SIGNAL(activated()), this, SLOT(on_MoveUp_activated()));
    MoveDownShortcut = new QShortcut(QKeySequence(Qt::Key_Down), this);
    connect(MoveDownShortcut, SIGNAL(activated()), this, SLOT(on_MoveDown_activated()));
    ZoomInShortcut = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
    ZoomInShortcut->setContext(Qt::WidgetShortcut);
    connect(ZoomInShortcut, SIGNAL(activated()), this, SLOT(zoomIn()));
    ZoomOutShortcut = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
    ZoomOutShortcut->setContext(Qt::WidgetShortcut);
    connect(ZoomOutShortcut, SIGNAL(activated()), this, SLOT(zoomOut()));

    QVBoxLayout* vlay = new QVBoxLayout(this);

    QHBoxLayout* hlay1 = new QHBoxLayout();
    p->TL = new QLabel(this);
    hlay1->addWidget(p->TL);
    QSpacerItem* horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    hlay1->addItem(horizontalSpacer);
    p->TR = new QLabel(this);
    hlay1->addWidget(p->TR);
    vlay->addLayout(hlay1);

    QSpacerItem* verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Minimum, QSizePolicy::Expanding);
    vlay->addItem(verticalSpacer);

    QHBoxLayout* hlay2 = new QHBoxLayout();
    p->BL = new QLabel(this);
    hlay2->addWidget(p->BL);
    QSpacerItem* horizontalSpacer2 = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    hlay2->addItem(horizontalSpacer2);
    p->BR = new QLabel(this);
    hlay2->addWidget(p->BR);
    vlay->addLayout(hlay2);

    p->TL->setVisible(false);
    p->TR->setVisible(false);
    p->BL->setVisible(false);
    p->BR->setVisible(false);
}

MapView::~MapView()
{
    delete StaticBackground;
    delete StaticBuffer;
    delete p;
}

MainWindow *MapView::main()
{
    return Main;
}

void MapView::setDocument(IDocument* aDoc)
{
    p->theDocument = aDoc;

    setViewport(viewport(), rect());
}

IDocument *MapView::document()
{
    return p->theDocument;
}

void MapView::invalidate(bool updateStaticBuffer, bool updateMap)
{
    if (updateStaticBuffer) {
        p->resetTileCache(rect());
        if (p->theDocument) {
            p->tileCalc(rect());
    //            foreach (TILE_TYPE t, tiles) p->renderTile(this, t);
            if (tiles.size()) {
                p->renderGathering = QtConcurrent::map(tiles, RenderTile(this));
            }
        }
        p->theVectorPanDelta = QPoint(0, 0);
        SAFE_DELETE(StaticBackground)
    }
    if (p->theDocument && updateMap) {
        IMapWatermark* WatermarkAdapter = NULL;
        for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt) {
            if (ImgIt.get()->isVisible()) {
                ImgIt.get()->forceRedraw(*this, p->BackgroundOnlyVpTransform, rect());
                WatermarkAdapter = qobject_cast<IMapWatermark*>(ImgIt.get()->getMapAdapter());
            }
        }
        p->BackgroundOnlyVpTransform = QTransform();

        if (WatermarkAdapter) {
            p->TL->setAttribute(Qt::WA_NoMousePropagation);
            p->TL->setOpenExternalLinks(true);
            p->TL->setText(WatermarkAdapter->getLogoHtml());
    //        p->lblLogo->move(10, 10);
            p->TL->show();

            p->BR->setAttribute(Qt::WA_NoMousePropagation);
            p->BR->setOpenExternalLinks(true);
            p->BR->setWordWrap(true);
            p->BR->setText(WatermarkAdapter->getAttributionsHtml(p->Viewport, rect()));
            p->BR->setMinimumWidth(150);
            p->BR->setMaximumWidth(200);
            p->BR->setMaximumHeight(50);
            p->BR->show();
        } else {
            p->TL->setVisible(false);
            p->BR->setVisible(false);
        }
    }
    update();
}

void MapView::panScreen(QPoint delta)
{
    if (p->BackgroundOnlyPanZoom) {
        Coord cDelta = fromView(delta) - fromView(QPoint(0, 0));
        p->BackgroundOnlyVpTransform.translate(-cDelta.x(), -cDelta.y());
    } else {
        p->theVectorPanDelta += delta;

//        CoordBox r1, r2;
//        if (delta.x()) {
//            if (delta.x() < 0)
//                r1 = CoordBox(p->Viewport.bottomRight(), Coord(p->Viewport.topRight().x() - cDelta.x(), p->Viewport.topRight().y())); // OK
//            else
//                r1 = CoordBox(Coord(p->Viewport.bottomLeft().x() - cDelta.x(), p->Viewport.bottomLeft().y()), p->Viewport.topLeft()); // OK
//            p->invalidRects.push_back(r1);
//        }
//        if (delta.y()) {
//            if (delta.y() < 0)
//                r2 = CoordBox(Coord(p->Viewport.bottomLeft().x(), p->Viewport.bottomLeft().y() - cDelta.y()), p->Viewport.bottomRight()); // OK
//            else
//                r2 = CoordBox(p->Viewport.topLeft(), Coord( p->Viewport.bottomRight().x(), p->Viewport.topRight().y() - cDelta.y())); //NOK
//            p->invalidRects.push_back(r2);
//        }


        //qDebug() << "Inv rects size: " << p->invalidRects.size();
        //    qDebug() << "delta: " << delta;
        //qDebug() << "r1 : " << p->theTransform.map(p->theProjection.project(r1.topLeft())) << ", " << p->theTransform.map(p->theProjection.project(r1.bottomRight()));
        //qDebug() << "r2 : " << p->theTransform.map(p->theProjection.project(r2.topLeft())) << ", " << p->theTransform.map(p->theProjection.project(r2.bottomRight()));

        p->theTransform.translate((qreal)(delta.x())/p->theTransform.m11(), (qreal)(delta.y())/p->theTransform.m22());
        p->theInvertedTransform = p->theTransform.inverted();
        viewportRecalc(rect());
        if (p->theDocument) {
            p->tileCalc(rect());
//            foreach (TILE_TYPE t, tiles) p->renderTile(this, t);
            if (tiles.size()) {
                p->renderGathering = QtConcurrent::map(tiles, RenderTile(this));
            }
        }
    }

    for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt)
        ImgIt.get()->pan(delta);
    update();
}

void MapView::rotateScreen(QPoint center, qreal angle)
{
    p->theVectorRotation += angle;

    transformCalc(p->theTransform, p->theProjection, p->theVectorRotation, p->Viewport, rect());
    p->theInvertedTransform = p->theTransform.inverted();
    viewportRecalc(rect());
//    p->invalidRects.clear();
//    p->invalidRects.push_back(p->Viewport);

//    for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt)
//        ImgIt.get()->pan(delta);
    update();
}

void MapView::paintEvent(QPaintEvent * anEvent)
{
    if (!p->theDocument)
        return;

#ifndef NDEBUG
    QTime Start(QTime::currentTime());
#endif

    p->theFeatures.clear();

    QPainter P;
    P.begin(this);

    updateStaticBackground();

    P.drawPixmap(p->theVectorPanDelta, *StaticBackground);
    P.save();
    QTransform AlignTransform;
    for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt) {
        if (ImgIt.get()->isVisible()) {
            ImgIt.get()->drawImage(&P);
            AlignTransform = ImgIt.get()->getCurrentAlignmentTransform();
        }
    }
    P.restore();

    updateStaticBuffer();
    P.drawPixmap(p->theVectorPanDelta, *StaticBuffer);

    drawLatLonGrid(P);
    drawDownloadAreas(P);
    drawScale(P);

    if (p->theInteraction) {
        P.setRenderHint(QPainter::Antialiasing);
        p->theInteraction->paintEvent(anEvent, P);
    }

    if (Main)
        drawGPS(P);

    P.end();

#ifndef _MOBILE
    if (Main) {
        QString vpLabel = QString("%1,%2,%3,%4")
                                           .arg(viewport().bottomLeft().x(),0,'f',4)
                                           .arg(viewport().bottomLeft().y(),0, 'f',4)
                                           .arg(viewport().topRight().x(),0, 'f',4)
                                           .arg(viewport().topRight().y(),0,'f',4)
                                           ;
        if (!p->theProjection.projIsLatLong()) {
            QRectF pVp = p->theProjection.toProjectedRectF(viewport(), rect());
            vpLabel += " / " + QString("%1,%2,%3,%4")
                    .arg(pVp.bottomLeft().x(),0,'f',4)
                    .arg(pVp.bottomLeft().y(),0, 'f',4)
                    .arg(pVp.topRight().x(),0, 'f',4)
                    .arg(pVp.topRight().y(),0,'f',4)
                    ;
        }
        Main->ViewportStatusLabel->setText(vpLabel);

        Main->MeterPerPixelLabel->setText(tr("%1 m/pixel").arg(1/p->PixelPerM, 0, 'f', 2));
        if (!AlignTransform.isIdentity()) {
            QLineF l(0, 0, AlignTransform.dx(), AlignTransform.dy());
            l.translate(viewport().center());
            Main->AdjusmentMeterLabel->setVisible(true);
            qreal distance = Coord(l.p2()).distanceFrom(Coord(l.p1()))*1000;
            Main->AdjusmentMeterLabel->setText(tr("Align: %1m @ %2").arg(distance, 0, 'f', 2).arg(l.angle(), 0, 'f', 2) + QString::fromUtf8("°"));
        } else {
            Main->AdjusmentMeterLabel->setVisible(false);
        }
#ifndef NDEBUG
        QTime Stop(QTime::currentTime());
        Main->PaintTimeLabel->setText(tr("%1ms").arg(Start.msecsTo(Stop)));
#endif
    }
#endif
}

void MapView::drawScale(QPainter & P)
{

    if (!TEST_RFLAGS(RendererOptions::ScaleVisible))
        return;

    errno = 0;
    qreal Log = log10(200./p->PixelPerM);
    if (errno != 0)
        return;

    qreal RestLog = Log-floor(Log);
    if (RestLog < log10(2.))
        Log = floor(Log);
    else if (RestLog < log10(5.))
        Log = floor(Log)+log10(2.);
    else
        Log = floor(Log)+log10(5.);

    qreal Length = pow(10.,Log);
    QPointF P1(20,height()-20);
    QPointF P2(20+Length*p->PixelPerM,height()-20);
    P.fillRect(P1.x()-4, P1.y()-20-4, P2.x() - P1.x() + 4, 33, QColor(255, 255, 255, 128));
    P.setPen(QPen(QColor(0,0,0),2));
    P.drawLine(P1-QPointF(0,5),P1+QPointF(0,5));
    P.drawLine(P1,P2);
    if (Length < 1000)
        P.drawText(QRectF(P2-QPoint(200,40),QSize(200,30)),Qt::AlignRight | Qt::AlignBottom, QString(tr("%1 m")).arg(Length, 0, 'f', 0));
    else
        P.drawText(QRectF(P2-QPoint(200,40),QSize(200,30)),Qt::AlignRight | Qt::AlignBottom, QString(tr("%1 km")).arg(Length/1000, 0, 'f', 0));

    P.drawLine(P2-QPointF(0,5),P2+QPointF(0,5));
}

void MapView::drawGPS(QPainter & P)
{
    if (Main->gps() && Main->gps()->getGpsDevice()) {
        if (Main->gps()->getGpsDevice()->fixStatus() == QGPSDevice::StatusActive) {
            Coord vp(Main->gps()->getGpsDevice()->longitude(), Main->gps()->getGpsDevice()->latitude());
            QPoint g = toView(vp);
            QImage* pm = getSVGImageFromFile(":/Gps/Gps_Marker.svg", 32);
            P.drawImage(g - QPoint(16, 16), *pm);
        }
    }
}

bool testColor(const QImage& theImage, const QPoint& P, const QRgb& targetColor)
{
    if (!theImage.rect().contains(P)) return false;
    return (theImage.pixel(P) == targetColor);
}

void floodFill(QImage& theImage, const QPoint& P, const QRgb& targetColor, const QRgb& replaceColor)
{
    if (!testColor(theImage, P, targetColor)) return;

    QStack<QPoint> theStack;
    QPoint aP;
    QPainter theP(&theImage);
    theP.setPen(QPen(QColor::fromRgb(replaceColor), 1));
    theP.setBrush(Qt::NoBrush);

    theStack.push(P);
    while (!theStack.isEmpty()) {
        aP = theStack.pop();
        QPoint W = aP;
        QPoint E = aP;
        if (testColor(theImage, aP + QPoint(0, 1), targetColor))
            theStack.push(aP + QPoint(0, 1));
        if (testColor(theImage, aP + QPoint(0, -1), targetColor))
            theStack.push(aP + QPoint(0, -1));
        while (testColor(theImage, W + QPoint(-1, 0),targetColor) && W.x() > 0) {
            W += QPoint(-1, 0);
            if (testColor(theImage, W + QPoint(0, 1), targetColor))
                theStack.push(W + QPoint(0, 1));
            if (testColor(theImage, W + QPoint(0, -1), targetColor))
                theStack.push(W + QPoint(0, -1));
        }
        while (testColor(theImage, E + QPoint(1, 0), targetColor) && E.x() < theImage.width()-1) {
            E += QPoint(1, 0);
            if (testColor(theImage, E + QPoint(0, 1), targetColor))
                theStack.push(E + QPoint(0, 1));
            if (testColor(theImage, E + QPoint(0, -1), targetColor))
                theStack.push(E + QPoint(0, -1));
        }
        theP.drawLine(W, E);
    }
}

#define PARALLEL_LINES_NUM 5
#define MEDIAN_LINES_NUM 5

void MapView::drawLatLonGrid(QPainter & P)
{
    if (!TEST_RFLAGS(RendererOptions::LatLonGridVisible))
        return;

    QPointF origin(0., 0.);
    QPoint p1 = toView(origin);
    QPointF p2 = fromView(QPoint(p1.x()+width(), p1.y()-height()));
    CoordBox adjViewport(origin, p2);
    qreal lonInterval = adjViewport.lonDiff() / 4;
    qreal latInterval = adjViewport.latDiff() / 4;

    int prec = log10(lonInterval);
    if (!lonInterval || !latInterval) return; // avoid divide-by-zero
    qreal lonStart = qMax(int((p->Viewport.bottomLeft().x() - origin.x()) / lonInterval) * lonInterval, -COORD_MAX);
    if (lonStart != -COORD_MAX) {
        lonStart -= origin.x();
        if (lonStart<1)
            lonStart -= lonInterval;
    }
    qreal latStart = qMax(int(p->Viewport.bottomLeft().y() / latInterval) * latInterval, -COORD_MAX/2);
    if (latStart != -COORD_MAX/2) {
        latStart -= origin.y();
        if (latStart<1)
            latStart -= lonInterval;
    }

    QList<QPolygonF> medianLines;
    QList<QPolygonF> parallelLines;

    for (qreal y=latStart; y<=p->Viewport.topLeft().y()+latInterval; y+=latInterval) {
        QPolygonF l;
        for (qreal x=lonStart; x<=p->Viewport.bottomRight().x()+lonInterval; x+=lonInterval) {
            QPointF pt = p->theProjection.project(Coord(qMin(x, COORD_MAX), qMin(y, COORD_MAX/2)));
            l << pt;
        }
        parallelLines << l;
    }
    for (qreal x=lonStart; x<=p->Viewport.bottomRight().x()+lonInterval; x+=lonInterval) {
        QPolygonF l;
        for (qreal y=latStart; y<=p->Viewport.topLeft().y()+latInterval; y+=latInterval) {
            QPointF pt = p->theProjection.project(Coord(qMin(x, COORD_MAX), qMin(y, COORD_MAX/2)));
            l << pt;
        }
        medianLines << l;
    }

    P.save();
    P.setRenderHint(QPainter::Antialiasing);
    P.setPen(QColor(180, 217, 255));
    QLineF lb = QLineF(rect().topLeft(), rect().bottomLeft());
    QLineF lt = QLineF(rect().topLeft(), rect().topRight());
    QLineF l;
    for (int i=0; i<parallelLines.size(); ++i) {

        if (parallelLines[i].size() == 0)
          continue;

        P.drawPolyline(p->theTransform.map(parallelLines[i]));
        int k=0;
        QPointF pt;
        while (k < parallelLines.at(i).size()-2) {
            l = QLineF(p->theTransform.map(parallelLines.at(i).at(k)), p->theTransform.map(parallelLines.at(i).at(k+1)));
            if (l.intersect(lb, &pt) == QLineF::BoundedIntersection)
                break;
            ++k;
        }
        if (pt.isNull())
            continue;
//        QPoint pt = QPoint(0, p->theTransform.map(parallelLines.at(i).at(0)).y());
        QPoint ptt = pt.toPoint() + QPoint(5, -5);
        P.drawText(ptt, QString("%1").arg(p->theProjection.inverse2Coord(parallelLines.at(i).at(0)).y(), 0, 'f', 2-prec));
    }
    for (int i=0; i<medianLines.size(); ++i) {

        if (medianLines[i].size() == 0)
          continue;

        P.drawPolyline(p->theTransform.map(medianLines[i]));
        int k=0;
        QPointF pt;
        while (k < medianLines.at(i).size()-2) {
            l = QLineF(p->theTransform.map(medianLines.at(i).at(k)), p->theTransform.map(medianLines.at(i).at(k+1)));
            if (l.intersect(lt, &pt) == QLineF::BoundedIntersection)
                break;
            ++k;
        }
        if (pt.isNull())
            continue;
//        QPoint pt = QPoint(p->theTransform.map(medianLines.at(i).at(0)).x(), 0);
        QPoint ptt = pt.toPoint() + QPoint(5, 10);
        P.drawText(ptt, QString("%1").arg(p->theProjection.inverse2Coord(medianLines.at(i).at(0)).x(), 0, 'f', 2-prec));
    }

    P.restore();
}

void MapView::drawFeatures(QPainter & P)
{
//    for (int i=0; i<p->theDocument->layerSize(); ++i)
//        g_backend.getFeatureSet(p->theDocument->getLayer(i), p->theFeatures, p->invalidRects, p->theProjection);

//    p->renderer.render(&P, p->theFeatures, p->ROptions, this);

    p->renderGathering.waitForFinished();

    QPointF origin = p->theTransform.map(p->tileOriginCoord);
    for (int i=p->tileViewport.top(); i<=p->tileViewport.bottom(); ++i)
        for (int j=p->tileViewport.left(); j<=p->tileViewport.right(); ++j) {
            tileLock.lockForRead();
            if (tileCache.contains(TILE_CONSTRUCTOR(j, i))) {
                QPointF tl = QPointF((j*TILE_SIZE)+origin.x(), (i*TILE_SIZE)+origin.y());
                P.drawImage(tl, *(tileCache[TILE_CONSTRUCTOR(j, i)]));
            }
            tileLock.unlock();
//            qDebug() << QPoint(j, i) << tl;
        }

}

void MapView::drawDownloadAreas(QPainter & P)
{
    if (!TEST_RFLAGS(RendererOptions::DownloadedVisible))
        return;

    P.save();
    QRegion r(0, 0, width(), height());


    //QBrush b(Qt::red, Qt::DiagCrossPattern);
    QBrush b(Qt::red, Qt::Dense7Pattern);

    QList<CoordBox> db = p->theDocument->getDownloadBoxes();
    QList<CoordBox>::const_iterator bb;
    for (bb = db.constBegin(); bb != db.constEnd(); ++bb) {
        if (viewport().disjunctFrom(*bb)) continue;
        QPolygonF poly;
        poly << projection().project((*bb).topLeft());
        poly << projection().project((*bb).bottomLeft());
        poly << projection().project((*bb).bottomRight());
        poly << projection().project((*bb).topRight());
        poly << projection().project((*bb).topLeft());

        r -= QRegion(p->theTransform.map(poly.toPolygon()));
    }

    P.setClipRegion(r);
    P.setClipping(true);
    P.fillRect(rect(), b);

    P.restore();
}

void MapView::updateStaticBackground()
{
    if (!StaticBackground || (StaticBackground->size() != size()))
    {
        delete StaticBackground;
        StaticBackground = new QPixmap(size());

        if (M_PREFS->getUseShapefileForBackground())
            StaticBackground->fill(M_PREFS->getWaterColor());
        else if (M_PREFS->getBackgroundOverwriteStyle())
            StaticBackground->fill(M_PREFS->getBgColor());
        else if (M_STYLE->getGlobalPainter().getDrawBackground())
            StaticBackground->fill(M_STYLE->getGlobalPainter().getBackgroundColor());
        else
            StaticBackground->fill(Qt::white);
    }
}

void MapView::updateStaticBuffer()
{
//    QPainter P;

//    if (!p->theVectorPanDelta.isNull()) {
//#if QT_VERSION < 0x040600
//        QPixmap savPix;
//        savPix = StaticBuffer->copy();
//        StaticBuffer->fill(Qt::transparent);
//        P.begin(StaticBuffer);
//        P.drawPixmap(p->theVectorPanDelta, savPix);
//        P.setClipping(true);
//        P.setClipRegion(QRegion(rect()) - QRegion(QRect(p->theVectorPanDelta, size())));
//#else
//        QRegion exposed;
//        StaticBuffer->scroll(p->theVectorPanDelta.x(), p->theVectorPanDelta.y(), StaticBuffer->rect(), &exposed);
//        P.begin(StaticBuffer);
//        P.setClipping(true);
//        P.setClipRegion(exposed);
//        P.eraseRect(StaticBuffer->rect());
//#endif
//    } else {
//        StaticBuffer->fill(Qt::transparent);
//        P.begin(StaticBuffer);
//        P.setClipping(true);
//        P.setClipRegion(rect());
//    }

//    if (!p->invalidRects.isEmpty()) {
////        P.setRenderHint(QPainter::Antialiasing);
////        P.setClipping(true);
////        P.setClipRegion(QRegion(rect()));
//        if (M_PREFS->getUseAntiAlias())
//            if (p->invalidRects[0] == p->Viewport || M_PREFS->getAntiAliasWhilePanning())
//                P.setRenderHint(QPainter::Antialiasing);
//        drawFeatures(P);
//        P.end();
//    }

//    p->invalidRects.clear();

    QPainter P;

    StaticBuffer->fill(Qt::transparent);
    P.begin(StaticBuffer);
    P.setCompositionMode(QPainter::CompositionMode_Source);
    P.setClipping(true);
    P.setClipRegion(rect());
//    if (M_PREFS->getUseAntiAlias())
//        P.setRenderHint(QPainter::Antialiasing);
    drawFeatures(P);
    P.end();

    p->theVectorPanDelta = QPoint(0, 0);
}

void MapView::mousePressEvent(QMouseEvent* anEvent)
{
    if (p->theInteraction)
        if (anEvent->button())
            p->theInteraction->mousePressEvent(anEvent);
}

void MapView::mouseReleaseEvent(QMouseEvent* anEvent)
{
    if (p->theInteraction)
    p->theInteraction->mouseReleaseEvent(anEvent);

}

void MapView::mouseMoveEvent(QMouseEvent* anEvent)
{
    if (p->theInteraction)
    p->theInteraction->mouseMoveEvent(anEvent);
}

void MapView::mouseDoubleClickEvent(QMouseEvent* anEvent)
{
    if (p->theInteraction)
    p->theInteraction->mouseDoubleClickEvent(anEvent);
}

void MapView::wheelEvent(QWheelEvent* anEvent)
{
    if (p->theInteraction)
        p->theInteraction->wheelEvent(anEvent);
}

Interaction *MapView::interaction()
{
    return p->theInteraction;
}

void MapView::setInteraction(Interaction *anInteraction)
{
    p->theInteraction = anInteraction;
}

Projection& MapView::projection()
{
    return p->theProjection;
}

QTransform& MapView::transform()
{
    return p->theTransform;
}

QTransform& MapView::invertedTransform()
{
    return p->theInvertedTransform;
}

QPoint MapView::toView(const Coord& aCoord) const
{
    return p->theTransform.map(p->theProjection.project(aCoord)).toPoint();
}

QPoint MapView::toView(Node* aPt) const
{
    return p->theTransform.map(aPt->projected()).toPoint();
}

Coord MapView::fromView(const QPoint& aPt) const
{
    return p->theProjection.inverse2Coord(p->theInvertedTransform.map(QPointF(aPt)));
}


void MapView::on_imageReceived(ImageMapLayer* aLayer)
{
    aLayer->forceRedraw(*this, p->BackgroundOnlyVpTransform, rect());
    p->BackgroundOnlyVpTransform = QTransform();
    update();
}

void MapView::resizeEvent(QResizeEvent * ev)
{
    viewportRecalc(QRect(QPoint(0,0), ev->size()));

    QWidget::resizeEvent(ev);

    if (!StaticBuffer || (StaticBuffer->size() != size()))
    {
        delete StaticBuffer;
        StaticBuffer = new QPixmap(size());
    }

    invalidate(true, true);
}

void MapView::dragEnterEvent(QDragEnterEvent *event)
{
    if (!Main) {
        event->ignore();
        return;
    }
}

void MapView::dragMoveEvent(QDragMoveEvent *event)
{
    if (!Main) {
        event->ignore();
        return;
    }
}

void MapView::dropEvent(QDropEvent *event)
{
    if (!Main) {
        event->ignore();
        return;
    }
}

bool MapView::toXML(QXmlStreamWriter& stream)
{
    bool OK = true;

    stream.writeStartElement("MapView");
    viewport().toXML("Viewport", stream);
    p->theProjection.toXML(stream);
    stream.writeEndElement();

    return OK;
}

void MapView::fromXML(QXmlStreamReader& stream)
{
    CoordBox cb;
    stream.readNext();
    while(!stream.atEnd() && !stream.isEndElement()) {
        if (stream.name() == "Viewport") {
            cb = CoordBox::fromXML(stream);
        } else if (stream.name() == "Projection") {
            p->theProjection.fromXML(stream);
        }
        stream.readNext();
    }

    if (!cb.isNull())
        setViewport(cb, rect());
}

void MapView::on_MoveLeft_activated()
{
    QPoint p(rect().width()/4,0);
    panScreen(p);

//    invalidate(true, true);
}
void MapView::on_MoveRight_activated()
{
    QPoint p(-rect().width()/4,0);
    panScreen(p);

//    invalidate(true, true);
}

void MapView::on_MoveUp_activated()
{
    QPoint p(0,rect().height()/4);
    panScreen(p);

//    invalidate(true, true);
}

void MapView::on_MoveDown_activated()
{
    QPoint p(0,-rect().height()/4);
    panScreen(p);

//    invalidate(true, true);
}

void MapView::zoomIn()
{
    zoom(M_PREFS->getZoomIn()/100., rect().center());
}

void MapView::zoomOut()
{
    zoom(M_PREFS->getZoomOut()/100., rect().center());
}

bool MapView::isSelectionLocked()
{
    return SelectionLocked;
}

void MapView::lockSelection()
{
    if (!Main)
        return;

    if (!SelectionLocked && Main->properties()->selection().size()) {
#ifndef _MOBILE
        lockIcon = new QLabel(this);
        lockIcon->setPixmap(QPixmap(":/Icons/emblem-readonly.png"));
        Main->statusBar()->clearMessage();
        Main->statusBar()->addWidget(lockIcon);
#endif
        SelectionLocked = true;
    }
}

void MapView::unlockSelection()
{
    if (!Main)
        return;

    if (SelectionLocked) {
#ifndef _MOBILE
        Main->statusBar()->removeWidget(lockIcon);
        SAFE_DELETE(lockIcon)
#endif
        SelectionLocked = false;
    }
}

const CoordBox& MapView::viewport() const
{
    return p->Viewport;
}

void MapView::viewportRecalc(const QRect & Screen)
{
    Coord br = fromView(Screen.bottomRight());
    Coord tl = fromView(Screen.topLeft());
    p->Viewport = CoordBox(tl, br);

    // measure geographical distance between mid left and mid right of the screen
    int mid = (Screen.topLeft().y() + Screen.bottomLeft().y()) / 2;
    Coord left = fromView(QPoint(Screen.left(), mid));
    Coord right = fromView(QPoint(Screen.right(), mid));
    p->PixelPerM = Screen.width() / (left.distanceFrom(right)*1000);

    emit viewportChanged();
}

void MapView::transformCalc(QTransform& theTransform, const Projection& theProjection, const qreal& theRotation, const CoordBox& TargetMap, const QRect& screen)
{
    QRectF pViewport = theProjection.toProjectedRectF(TargetMap, screen);
//    QPointF pCenter(pViewport.center());

    qreal Aspect = (double)screen.width() / screen.height();
    qreal pAspect = fabs(pViewport.width() / pViewport.height());

    qreal wv, hv;
    if (pAspect > Aspect) {
        wv = fabs(pViewport.width());
        hv = fabs(pViewport.height() * pAspect / Aspect);
    } else {
        wv = fabs(pViewport.width() * Aspect / pAspect);
        hv = fabs(pViewport.height());
    }

    qreal ScaleLon = screen.width() / wv;
    qreal ScaleLat = screen.height() / hv;

//    qreal PLon = pCenter.x() /* * ScaleLon*/;
//    qreal PLat = pCenter.y() /* * ScaleLat*/;
//    qreal DeltaLon = Screen.width() / 2 - PLon;
//    qreal DeltaLat = Screen.height() - (Screen.height() / 2 - PLat);

//    theTransform.setMatrix(ScaleLon, 0, 0, 0, -ScaleLat, 0, DeltaLon, DeltaLat, 1);
    theTransform.reset();
    theTransform.scale(ScaleLon, -ScaleLat);
//    theTransform.rotate(theRotation, Qt::ZAxis);
    theTransform.translate(-pViewport.topLeft().x(), -pViewport.topLeft().y());
//    theTransform.translate(-pCenter.x(), -pCenter.y());
//    QLineF l(QPointF(0, 0), pCenter);
//    l.setAngle(l.angle()+theRotation);
//    qDebug() << "p2:" << l.p2();
//    theTransform.translate(l.p2().x(), l.p2().y());
//    theTransform.translate(Screen.width()/2, -Screen.height()/2);
//    theTransform.rotateRadians(theRotation);
}

void MapView::setViewport(const CoordBox & TargetMap,
                                    const QRect & Screen)
{
    CoordBox targetVp;
    if (TargetMap.latDiff() == 0 || TargetMap.lonDiff() == 0)
        targetVp = CoordBox (TargetMap.center()-COORD_ENLARGE*10, TargetMap.center()+COORD_ENLARGE*10);
    else
        targetVp = TargetMap;
    transformCalc(p->theTransform, p->theProjection, p->theVectorRotation, targetVp, Screen);
    p->theInvertedTransform = p->theTransform.inverted();
    viewportRecalc(Screen);

    p->ZoomLevel = p->theTransform.m11();

    if (TEST_RFLAGS(RendererOptions::LockZoom) && p->theDocument) {
        ImageMapLayer* l = NULL;
        for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt) {
            l = ImgIt.get();
            break;
        }
        if (l && l->isTiled()) {
            l->setCurrentZoom(*this, p->Viewport, rect());
            qreal pixpercoord = width() / p->Viewport.lonDiff();
            qreal z = l->pixelPerCoord() / pixpercoord;
            zoom(z, Screen.center(), Screen);
        }
    }
    invalidate(true, true);
}

void MapView::zoom(qreal d, const QPoint & Around)
{
    // Sensible limits on zoom range (circular scroll on touchpads can scroll
    // very fast).
    if (p->PixelPerM * d > 100 && d > 1.0)
        return;
    if (p->PixelPerM * d < 1e-5 && d < 1.0)
        return;

    qreal z = d;
    if (TEST_RFLAGS(RendererOptions::LockZoom)) {
        ImageMapLayer* l = NULL;
        for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt) {
            l = ImgIt.get();
            break;
        }
        if (l && l->isTiled()) {
            if (d > 1.0) {
                l->zoom_in();
            } else {
                l->zoom_out();
            }
            qreal pixpercoord = width() / p->Viewport.lonDiff();
            z = l->pixelPerCoord() / pixpercoord;
        }
    }

    zoom(z, Around, rect());
    p->resetTileCache(rect());
    if (p->theDocument) {
        p->tileCalc(rect());
//            foreach (TILE_TYPE t, tiles) p->renderTile(this, t);
        if (tiles.size()) {
            p->renderGathering = QtConcurrent::map(tiles, RenderTile(this));
        }
    }
}

void MapView::zoom(qreal d, const QPoint & Around,
                             const QRect & Screen)
{
    QPointF pBefore = p->theInvertedTransform.map(QPointF(Around));

    qreal ScaleLon = p->theTransform.m11() * d;
    qreal ScaleLat = p->theTransform.m22() * d;
    qreal DeltaLat = (Around.y() - pBefore.y() * ScaleLat);
    qreal DeltaLon = (Around.x() - pBefore.x() * ScaleLon);

//    p->theTransform.setMatrix(ScaleLon*cos(p->theVectorRotation), 0, 0, 0, ScaleLat*cos(p->theVectorRotation), 0, DeltaLon, DeltaLat, 1);
    p->theTransform.reset();
    p->theTransform.scale(ScaleLon, ScaleLat);
//    p->theTransform.rotate(p->theVectorRotation, Qt::ZAxis);
    p->theTransform.translate(DeltaLon/ScaleLon, DeltaLat/ScaleLat);

    p->theInvertedTransform = p->theTransform.inverted();
    viewportRecalc(Screen);

    for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt)
        ImgIt.get()->zoom(d, Around, Screen);

    p->ZoomLevel = ScaleLon;

//    QPointF pt = p->theProjection.project(Coord(0, angToInt(180)));
//    qreal earthWidth = pt.x() * 2;
//    qreal zoomPixPerMat0 = 512. / earthWidth;
//    qreal z = 0;
//    p->AbstractZoomLevel = 0;
//    for (;z<p->theTransform.m11(); ++p->AbstractZoomLevel) {
//        qreal zoomPixPerMatCur = zoomPixPerMat0 * pow(2., p->AbstractZoomLevel);
//        z = zoomPixPerMatCur / p->PixelPerM;
//    }
    update();
}

void MapView::adjustZoomToBoris()
{
    if (TEST_RFLAGS(RendererOptions::LockZoom)) {
        ImageMapLayer* l = NULL;
        for (LayerIterator<ImageMapLayer*> ImgIt(p->theDocument); !ImgIt.isEnd(); ++ImgIt) {
            l = ImgIt.get();
            break;
        }
        if (l && l->isTiled()) {
            qreal pixpercoord = width() / p->Viewport.lonDiff();
            qreal z = l->pixelPerCoord() / pixpercoord;
            zoom(z, rect().center(), rect());
        }
    }
}

void MapView::setCenter(Coord & Center, const QRect & /*Screen*/)
{
    Coord curCenter(p->Viewport.center());
    QPoint curCenterScreen = toView(curCenter);
    QPoint newCenterScreen = toView(Center);

    QPoint panDelta = (curCenterScreen - newCenterScreen);
    panScreen(panDelta);
}

qreal MapView::pixelPerM() const
{
    return p->PixelPerM;
}

void MapView::setBackgroundOnlyPanZoom(bool val)
{
    p->BackgroundOnlyPanZoom = val;
}

RendererOptions MapView::renderOptions()
{
    return p->ROptions;
}

void MapView::setRenderOptions(const RendererOptions &opt)
{
    p->ROptions = opt;
}

QString MapView::toPropertiesHtml()
{
    QString h;

    h += "<big><strong>" + tr("View") + "</strong></big><hr/>";
    h += "<u>" + tr("Bounding Box") + "</u><br/>";
    h += QString("%1, %2, %3, %4 (%5, %6, %7, %8)")
         .arg(QString::number(viewport().bottomLeft().x()),'f',4)
         .arg(QString::number(viewport().bottomLeft().y()),'f',4)
         .arg(QString::number(viewport().topRight().x()),'f',4)
         .arg(QString::number(viewport().topRight().y()),'f',4)
         .arg(Coord2Sexa(viewport().bottomLeft().x()))
         .arg(Coord2Sexa(viewport().bottomLeft().y()))
         .arg(Coord2Sexa(viewport().topRight().x()))
         .arg(Coord2Sexa(viewport().topRight().y()))
         ;
    h += "<br/>";
    h += "<u>" + tr("Projection") + "</u><br/>";
    h += p->theProjection.getProjectionType();
    h += "";

    return h;
}
