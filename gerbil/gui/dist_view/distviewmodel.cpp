/*
	Copyright(c) 2012 Johannes Jordan <johannes.jordan@cs.fau.de>.
	Copyright(c) 2012 Petr Koupy <petr.koupy@gmail.com>

	This file may be licensed under the terms of of the GNU General Public
	License, version 3, as published by the Free Software Foundation. You can
	find it here: http://www.gnu.org/licenses/gpl.html
*/

#include "distviewmodel.h"
#include "../tasks/distviewbinstbb.h"
#include "viewer_tasks.h"

#include <background_task/background_task_queue.h>
#include <stopwatch.h>

#include <opencv2/core/core.hpp>
#include <iostream>

using namespace std;

DistViewModel::DistViewModel(representation::t type)
	: type(type), queue(NULL),
	  ignoreLabels(false)
{}

std::pair<multi_img_base::Value, multi_img_base::Value> DistViewModel::getRange()
{
	SharedDataLock ctxlock(context->mutex);
	return std::make_pair((*context)->minval, (*context)->maxval);
}

void DistViewModel::setLabelColors(QVector<QColor> colors)
{
	labelColors = colors; // TODO: maybe not threadsafe!
}

void DistViewModel::setIlluminant(cv::Mat1f illum)
{
	// illuminant should not affect binning, so no re-calculation
	// this is strange stuff. recheck, why illuminant sometimes is needed..
	illuminant = illum;
}

/*********   B I N N I N G   C A L C U L A T I O N S   **********/

void DistViewModel::updateBinning(int bins)
{
	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	if (bins > 0) {
		args.nbins = bins;
	}

	args.minvalValid = false;
	args.maxvalValid = false;
	args.binsizeValid = false;

	args.reset.fetch_and_store(1);
	args.wait.fetch_and_store(1);

	if (!image.get())
		return;

	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context, binsets));
	QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
					 this, SLOT(propagateBinning(bool)), Qt::QueuedConnection);
	queue->push(taskBins);
}

void DistViewModel::toggleLabels(bool toggle)
{
	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	ignoreLabels = toggle;
	args.ignoreLabels = toggle;
	args.wait.fetch_and_store(1);

	if (!image.get())
		return;

	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context, binsets));
	QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
					 this, SLOT(propagateBinning(bool)), Qt::QueuedConnection);
	queue->push(taskBins);
}

void DistViewModel::updateLabels(const cv::Mat1s &newLabels,
									const QVector<QColor> &colors)
{
	if (!newLabels.empty())
		labels = newLabels.clone();

	// check if we are ready to compute anything
	// TODO: also exit when in roi change. how to propagate this properly?
	if (!image.get() || labels.empty())
		return;

	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	args.wait.fetch_and_store(1);

	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context,
								   binsets));
	QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
					 this, SLOT(propagateBinning(bool)), Qt::QueuedConnection);
	queue->push(taskBins);
}

void DistViewModel::updateLabelsPartially(const cv::Mat1s &newLabels,
											 const cv::Mat1b &mask)
{
	// save old configuration for partial updates
	cv::Mat1s oldLabels = labels.clone();
	// just override the whole thing
	labels = newLabels.clone();

	if (!image.get())
		return;

	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	args.wait.fetch_and_store(1);

	// we calculate into temp, then from temp in the second round
	sets_ptr temp(new SharedData<std::vector<BinSet> >(NULL));

	{	// first round: delete all pixels from their *previous* labels
		std::vector<cv::Rect> sub;
		sub.push_back(cv::Rect(0, 0, mask.cols, mask.rows));
		BackgroundTaskPtr taskBins(new DistviewBinsTbb(
			image, oldLabels, labelColors, illuminant, args,
			context, binsets, temp, sub, std::vector<cv::Rect>(),
			mask, false, false));
		queue->push(taskBins);
	}

	{	// second round: now add them back according to their current labels
		/* we do not clone labels, as labels is never changed, only replaced
		 * TODO: write that down at the appropriate place so people will
		 * adhere to that */
		std::vector<cv::Rect> add;
		add.push_back(cv::Rect(0, 0, mask.cols, mask.rows));
		BackgroundTaskPtr taskBins(new DistviewBinsTbb(
			image, labels, labelColors, illuminant, args,
			context, binsets, temp, std::vector<cv::Rect>(), add,
			mask, false, true));

		// final signal
		QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
				 this, SLOT(propagateBinning(bool)), Qt::QueuedConnection);
		queue->push(taskBins);
	}
}


void DistViewModel::subPixels(const std::map<std::pair<int, int>, short> &points)
{
	std::vector<cv::Rect> sub(points.size());
	std::map<std::pair<int, int>, short>::const_iterator it;
	for (it = points.begin(); it != points.end(); ++it) {
		sub.push_back(cv::Rect(it->first.first, it->first.second, 1, 1));
	}

	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	BackgroundTaskPtr taskSub(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context, binsets,
		sets_ptr(new SharedData<std::vector<BinSet> >(NULL)), sub, std::vector<cv::Rect>(), cv::Mat1b(), true, false));
	queue->push(taskSub);
	taskSub->wait(); // TODO: why do we wait?

	/* Note: subPixels() is always first step before addPixels(). So finished
	 * operation is only signalled at the end of addPixels() */
}

void DistViewModel::addPixels(const std::map<std::pair<int, int>, short> &points)
{
	std::vector<cv::Rect> add(points.size());
	std::map<std::pair<int, int>, short>::const_iterator it;
	for (it = points.begin(); it != points.end(); ++it) {
		add.push_back(cv::Rect(it->first.first, it->first.second, 1, 1));
	}

	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	BackgroundTaskPtr taskAdd(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context, binsets,
		sets_ptr(new SharedData<std::vector<BinSet> >(NULL)), std::vector<cv::Rect>(), add, cv::Mat1b(), true, false));
	queue->push(taskAdd);
	bool success = taskAdd->wait(); // TODO: why do we wait?

	propagateBinning(success);
}

void DistViewModel::subImage(sets_ptr temp,
								const std::vector<cv::Rect> &regions,
								cv::Rect roi)
{
	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context,
		binsets,	temp, regions,
		std::vector<cv::Rect>(), cv::Mat1b(), false, false, roi));
	queue->push(taskBins);
}

void DistViewModel::addImage(sets_ptr temp,
								const std::vector<cv::Rect> &regions,
								cv::Rect roi)
{
	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	args.labelsValid = false;
	args.metaValid = false;
	args.minvalValid = false;
	args.maxvalValid = false;
	args.binsizeValid = false;

	args.reset.fetch_and_store(1);
	args.wait.fetch_and_store(1);

	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context,
		binsets, temp, std::vector<cv::Rect>(), regions,
		cv::Mat1b(), false, true, roi));
	// connect to propagateBinningRange as this operation can change range
	QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
					 this, SLOT(propagateBinningRange(bool)));
	queue->push(taskBins);
}

void DistViewModel::setImage(SharedMultiImgPtr img, cv::Rect roi, int bins)
{
	image = img;
	//GGDBGM(format("image.get()=%1%\n") %image.get());

	SharedDataLock ctxlock(context->mutex);
	ViewportCtx args = **context;
	ctxlock.unlock();

	args.type = type;
	args.ignoreLabels = ignoreLabels;

	args.nbins = bins;

	args.dimensionalityValid = false;
	args.labelsValid = false;
	args.metaValid = false;
	args.minvalValid = false;
	args.maxvalValid = false;
	args.binsizeValid = false;

	args.reset.fetch_and_store(1);
	args.wait.fetch_and_store(1);

	assert(context);
	BackgroundTaskPtr taskBins(new DistviewBinsTbb(
		image, labels, labelColors, illuminant, args, context, binsets,
		sets_ptr(new SharedData<std::vector<BinSet> >(NULL)), std::vector<cv::Rect>(),
		std::vector<cv::Rect>(), cv::Mat1b(), false, true, roi));
	// connect to propagateBinningRange, new image may have new range
	QObject::connect(taskBins.get(), SIGNAL(finished(bool)),
					 this, SLOT(propagateBinningRange(bool)));
	queue->push(taskBins);
}

void DistViewModel::propagateBinning(bool updated)
{
	if (!updated || !image.get())
		return;
	emit newBinning(type);
}

void DistViewModel::propagateBinningRange(bool updated)
{
	if (!updated || !image.get())
		return;
	emit newBinningRange(type);
}

/*********   H I G H L I G H T   M A S K   **********/

void DistViewModel::clearMask()
{
	SharedDataLock imagelock(image->mutex);
	highlightMask = cv::Mat1b((*image)->height, (*image)->width, (uchar)0);
}

/* create mask from single-band user selection */
void DistViewModel::fillMaskSingle(int dim, int sel)
{
	SharedDataLock imagelock(image->mutex);
	SharedDataLock ctxlock(context->mutex);
	fillMaskSingleBody body(highlightMask, (**image)[dim], dim, sel,
		(*context)->minval, (*context)->binsize, illuminant);
	tbb::parallel_for(tbb::blocked_range2d<size_t>(
		0, highlightMask.rows, 0, highlightMask.cols), body);
}

/* create mask from multi-band range selection */
void DistViewModel::fillMaskLimiters(const std::vector<std::pair<int, int> >& l)
{
	SharedDataLock imagelock(image->mutex);
	SharedDataLock ctxlock(context->mutex);
	fillMaskLimitersBody body(highlightMask, **image, (*context)->minval,
		(*context)->binsize, illuminant, l);
	tbb::parallel_for(tbb::blocked_range2d<size_t>(
		0,(*image)->height, 0, (*image)->width), body);
}

/* update mask according to user change in one band */
void DistViewModel::updateMaskLimiters(
		const std::vector<std::pair<int, int> >& l, int dim)
{
	SharedDataLock imagelock(image->mutex);
	SharedDataLock ctxlock(context->mutex);
	updateMaskLimitersBody body(highlightMask, **image, dim, (*context)->minval,
		(*context)->binsize, illuminant, l);
	tbb::parallel_for(tbb::blocked_range2d<size_t>(
		0,(*image)->height, 0, (*image)->width), body);
}

/*********   P I X E L   O V E R L A Y   **********/

QPolygonF DistViewModel::getPixelOverlay(int y, int x)
{
	SharedDataLock imagelock(image->mutex);
	SharedDataLock ctxlock(context->mutex);
	if (y >= (*image)->height ||
		x >= (*image)->width)
		return QPolygonF();

	const multi_img::Pixel &pixel = (**image)(y, x);
	QPolygonF points((*image)->size());

	for (unsigned int d = 0; d < (*image)->size(); ++d) {
		points[d] = QPointF(d, Compute::curpos(pixel[d], d,
			(*context)->minval, (*context)->binsize, illuminant));
	}
	return points;
}