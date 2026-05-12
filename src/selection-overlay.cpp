#include "selection-overlay.hpp"

#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QPen>
#include <QColor>
#include <QFont>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QEventLoop>
#include <QtGlobal>

static QRect computeVirtualGeometry()
{
	// Union of all screens' geometries (in device-independent virtual coords).
	QRect r;
	const auto screens = QApplication::screens();
	for (const QScreen *s : screens)
		r = r.united(s->geometry());
	if (r.isEmpty()) {
		QScreen *primary = QApplication::primaryScreen();
		if (primary)
			r = primary->geometry();
	}
	return r;
}

SelectionOverlay::SelectionOverlay(QWidget *parent)
	: QWidget(parent,
		  Qt::Window | Qt::FramelessWindowHint |
			  Qt::WindowStaysOnTopHint | Qt::Tool |
			  Qt::BypassWindowManagerHint)
{
	setAttribute(Qt::WA_DeleteOnClose, false);
	setAttribute(Qt::WA_TranslucentBackground, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	setMouseTracking(true);
	setCursor(Qt::CrossCursor);
	setFocusPolicy(Qt::StrongFocus);

	m_virtualGeometry = computeVirtualGeometry();
	setGeometry(m_virtualGeometry);
}

bool SelectionOverlay::runModal()
{
	m_accepted = false;
	m_dragging = false;
	m_result = QRect();

	show();
	raise();
	activateWindow();
	setFocus(Qt::OtherFocusReason);

	QEventLoop loop;
	connect(this, &QObject::destroyed, &loop, &QEventLoop::quit);

	// Poll-style: just spin until the widget is hidden by close()
	while (isVisible())
		loop.processEvents(QEventLoop::AllEvents, 16);

	return m_accepted;
}

void SelectionOverlay::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, false);

	// Dim the entire screen with a translucent dark fill.
	p.fillRect(rect(), QColor(14, 17, 22, 110));  // ~43% alpha

	// Hint text (top-left).
	{
		QFont f = p.font();
		f.setPointSize(13);
		f.setBold(true);
		p.setFont(f);
		p.setPen(QColor(255, 255, 255, 230));
		p.drawText(QRect(24, 18, width() - 48, 36),
			   Qt::AlignLeft | Qt::AlignVCenter,
			   QStringLiteral("드래그하여 캡처할 영역을 지정 — ESC 취소  |  "
					  "Drag to select a region — ESC to cancel"));
	}

	if (!m_dragging)
		return;

	// Compute selection rect in widget coords.
	const QRect sel = QRect(m_anchor, m_cursor).normalized();

	// Punch the dim layer where the selection is so it looks "lit".
	p.setCompositionMode(QPainter::CompositionMode_Clear);
	p.fillRect(sel, Qt::transparent);
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);

	// White outer border + red inner border for visibility on any background.
	QPen white(QColor(255, 255, 255, 230));
	white.setWidth(4);
	p.setPen(white);
	p.setBrush(Qt::NoBrush);
	p.drawRect(sel);

	QPen red(QColor(255, 59, 59, 255));
	red.setWidth(2);
	p.setPen(red);
	p.drawRect(sel);

	// Size badge near the bottom-right of the selection.
	const QString sizeText =
		QStringLiteral("%1 × %2").arg(sel.width()).arg(sel.height());
	const QRect badge(sel.right() - 110, sel.bottom() + 6, 110, 22);
	p.fillRect(badge, QColor(0, 0, 0, 180));
	p.setPen(QColor(255, 255, 255));
	{
		QFont f = p.font();
		f.setPointSize(10);
		f.setBold(false);
		p.setFont(f);
	}
	p.drawText(badge, Qt::AlignCenter, sizeText);
}

void SelectionOverlay::mousePressEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
		return;
	m_dragging = true;
	m_anchor = e->pos();
	m_cursor = e->pos();
	update();
}

void SelectionOverlay::mouseMoveEvent(QMouseEvent *e)
{
	if (!m_dragging)
		return;
	m_cursor = e->pos();
	update();
}

void SelectionOverlay::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton || !m_dragging)
		return;
	m_dragging = false;
	m_cursor = e->pos();

	const QRect sel = QRect(m_anchor, m_cursor).normalized();

	// Reject zero-area selections; treat as cancel.
	if (sel.width() < 4 || sel.height() < 4) {
		m_accepted = false;
		m_result = QRect();
	} else {
		// Translate widget coords → virtual desktop coords.
		m_result = sel.translated(m_virtualGeometry.topLeft());
		m_accepted = true;
	}

	close();
}

void SelectionOverlay::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape) {
		m_accepted = false;
		m_result = QRect();
		close();
		return;
	}
	QWidget::keyPressEvent(e);
}
