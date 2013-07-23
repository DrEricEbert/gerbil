#ifndef DOCKCONTROLLER_H
#define DOCKCONTROLLER_H

#include "../common/background_task.h"
#include "model/representation.h"
#include "../common/shared_data.h"

#include <QObject>
#include <QPixmap>

class ImageModel;
class IllumModel;
class FalseColorModel;
class UsSegmentationModel;
class LabelingModel;

class Controller;

class MainWindow;

class BandDock;
class LabelingDock;
class NormDock;
class ROIDock;
class RgbDock;
class IllumDock;
class GraphSegWidget;
class UsSegmentationDock;
class LabelDock;

namespace vole
{
	class GraphSegConfig;
}

#include <QObject>

class DockController : public QObject
{
	Q_OBJECT
public:
	explicit DockController(Controller *chief);
	void init();

signals:
	void rgbRequested();
	// these are send to the graphSegModel
	void requestGraphseg(representation::t type,
						 cv::Mat1s seedMap,
						 const vole::GraphSegConfig &config,
						 bool resetLabel);
	void requestGraphsegBand(representation::t type, int bandId,
							 cv::Mat1s seedMap,
							 const vole::GraphSegConfig &config,
							 bool resetLabel);
public slots:
	void setGUIEnabled(bool enable, TaskType tt);

protected slots:
	// these are requested by the graphSegWidget
	void requestGraphseg(representation::t,
						 const vole::GraphSegConfig &config,
						 bool resetLabel);
	void requestGraphsegCurBand(const vole::GraphSegConfig &config,
								bool resetLabel);
	void highlightSingleLabel(short label, bool highlight);
protected:
	/* Create dock widget objects. */
	void createDocks();

	/* Initialize signal/slot connections. */
	void setupDocks();

	Controller* chief;

	BandDock *bandDock;
	LabelingDock *labelingDock;
	NormDock *normDock;
	ROIDock *roiDock;
	RgbDock *rgbDock;
	IllumDock *illumDock;
	UsSegmentationDock *usSegDock;
	LabelDock *labelDock;
};

#endif // DOCKCONTROLLER_H

