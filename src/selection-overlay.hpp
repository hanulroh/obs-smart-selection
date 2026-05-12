/*
Smart Selection — fullscreen drag overlay (Qt6).
Spans the entire virtual desktop (all monitors). User clicks-and-drags to
select a rectangle. Returned coordinates are in virtual desktop coordinates.
*/

#pragma once

#include <QWidget>
#include <QRect>
#include <QPoint>

class QPaintEvent;
class QMouseEvent;
class QKeyEvent;

class SelectionOverlay : public QWidget {
	Q_OBJECT

public:
	explicit SelectionOverlay(QWidget *parent = nullptr);
	~SelectionOverlay() override = default;

	// Show the overlay modally, block until the user releases the mouse
	// or presses Escape. Returns true if a region was selected; false on cancel.
	bool runModal();

	// Result is in *virtual desktop* pixel coordinates (i.e. relative to the
	// virtual screen origin, which may be negative on multi-monitor setups).
	QRect resultRect() const { return m_result; }

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	bool   m_dragging = false;
	QPoint m_anchor;          // local widget coords
	QPoint m_cursor;          // local widget coords (live)
	QRect  m_result;          // virtual-desktop coords
	bool   m_accepted = false;

	// Cached virtual desktop geometry (origin & size).
	QRect  m_virtualGeometry;
};
