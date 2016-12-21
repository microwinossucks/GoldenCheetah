/*
 * Copyright (c) 2016 Damien Grauser (Damien.Grauser@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "WebPageWindow.h"

#include "MainWindow.h"
#include "RideItem.h"
#include "RideFile.h"
#include "IntervalItem.h"
#include "IntervalTreeView.h"
#include "SmallPlot.h"
#include "Context.h"
#include "Athlete.h"
#include "Zones.h"
#include "Settings.h"
#include "Colors.h"
#include "Units.h"
#include "TimeUtils.h"
#include "HelpWhatsThis.h"

#ifdef NOWEBKIT
#include <QtWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineDownloadItem>
#endif

// overlay helper
#include "TabView.h"
#include "GcOverlayWidget.h"
#include "IntervalSummaryWindow.h"
#include <QDebug>

#ifdef NOWEBKIT
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineSettings>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineUrlRequestJob>

// request interceptor to get downloads in QtWebEngine
class WebDownloadInterceptor : public QWebEngineUrlRequestInterceptor
{
    public:
        WebDownloadInterceptor() : QWebEngineUrlRequestInterceptor(Q_NULLPTR) {}

    public slots:
        void interceptRequest(QWebEngineUrlRequestInfo &info) {
            //qDebug()<<info.requestUrl().toString();
        }
};

// custom scheme handler
class WebSchemeHandler : public QWebEngineUrlSchemeHandler
{
    public:
        WebSchemeHandler() : QWebEngineUrlSchemeHandler(Q_NULLPTR) {}
    public slots:
        void requestStarted(QWebEngineUrlRequestJob *request) {
            //qDebug()<<"urlscheme request"<<request->requestUrl().toString();
        }
};

#endif

// declared in main, we only want to use it to get QStyle
extern QApplication *application;

WebPageWindow::WebPageWindow(Context *context) : GcChartWindow(context), context(context), firstShow(true)
{
    //
    // reveal controls widget
    //

    // layout reveal controls
    QHBoxLayout *revealLayout = new QHBoxLayout;
    revealLayout->setContentsMargins(0,0,0,0);

    rButton = new QPushButton(application->style()->standardIcon(QStyle::SP_ArrowRight), "", this);
    rCustomUrl = new QLineEdit(this);
    revealLayout->addStretch();
    revealLayout->addWidget(rButton);
    revealLayout->addWidget(rCustomUrl);
    revealLayout->addStretch();

    connect(rCustomUrl, SIGNAL(returnPressed()), this, SLOT(userUrl()));
    connect(rButton, SIGNAL(clicked(bool)), this, SLOT(userUrl()));

    setRevealLayout(revealLayout);

    //
    // Chart settings
    //

    QWidget *settingsWidget = new QWidget(this);
    settingsWidget->setContentsMargins(0,0,0,0);
    //HelpWhatsThis *helpSettings = new HelpWhatsThis(settingsWidget);
    //settingsWidget->setWhatsThis(helpSettings->getWhatsThisText(HelpWhatsThis::ChartRides_Critical_MM_Config_Settings));


    QFormLayout *commonLayout = new QFormLayout(settingsWidget);
    customUrlLabel = new QLabel(tr("URL"));
    customUrl = new QLineEdit(this);
    customUrl->setFixedWidth(250);
    customUrl->setText("");

    commonLayout->addRow(customUrlLabel, customUrl);
    commonLayout->addRow(new QLabel("Hit return to apply URL"));

    setControls(settingsWidget);

    setContentsMargins(0,0,0,0);
    layout = new QVBoxLayout();
    layout->setSpacing(0);
    layout->setContentsMargins(2,0,2,2);
    setChartLayout(layout);

#ifdef NOWEBKIT
    view = new QWebEngineView(this);
    connect(view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));

    // set user agent to look like Chrome
    view->page()->profile()->setHttpUserAgent("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/52.0.2743.82 Safari/537.36");

    // add a download interceptor
    WebDownloadInterceptor *interceptor = new WebDownloadInterceptor;
    view->page()->profile()->setRequestInterceptor(interceptor);

    // cookies and storage
    view->page()->profile()->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    view->page()->profile()->setCachePath("/tmp");
    view->page()->profile()->setPersistentStoragePath("/tmp");

    // web scheme handler
    WebSchemeHandler *handler = new WebSchemeHandler;
    //view->page()->profile()->installUrlSchemeHandler("filesystem:https", handler);
    //view->page()->profile()->installUrlSchemeHandler("filesystem:http", handler);

    // add some settings
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);

#else
    view = new QWebView();
    connect(view, SIGNAL(loadFinished(bool)), this, SLOT(loadFinished(bool)));
#endif
    view->setPage(new simpleWebPage());
    view->setContentsMargins(0,0,0,0);
    view->page()->view()->setContentsMargins(0,0,0,0);
    view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    view->setAcceptDrops(false);
    layout->addWidget(view);

    HelpWhatsThis *help = new HelpWhatsThis(view);
    view->setWhatsThis(help->getWhatsThisText(HelpWhatsThis::ChartRides_Map));

    // if we change in settings, force replot by pressing return
    connect(customUrl, SIGNAL(returnPressed()), this, SLOT(forceReplot()));

    first = true;
    configChanged(CONFIG_APPEARANCE);

    // intercept downloads
#ifdef NOWEBKIT
    connect(view->page()->profile(), SIGNAL(downloadRequested(QWebEngineDownloadItem*)), this, SLOT(downloadRequested(QWebEngineDownloadItem*)));
    connect(view->page(), SIGNAL(linkHovered(QString)), this, SLOT(linkHovered(QString)));
#else
    view->page()->setForwardUnsupportedContent(true);
    connect(view->page(),SIGNAL(downloadRequested(QNetworkRequest)),this,SLOT(download(QNetworkRequest)));
    connect(view->page(),SIGNAL(unsupportedContent(QNetworkReply*)),this,SLOT(unsupportedContent(QNetworkReply*)));
#endif
}

WebPageWindow::~WebPageWindow()
{
}

void 
WebPageWindow::configChanged(qint32)
{

    // tinted palette for headings etc
    QPalette palette;
    palette.setBrush(QPalette::Window, QBrush(GColor(CPLOTBACKGROUND)));
    palette.setColor(QPalette::WindowText, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Text, GColor(CPLOTMARKER));
    palette.setColor(QPalette::Base, GCColor::alternateColor(GColor(CPLOTBACKGROUND)));
    setPalette(palette);
}

void
WebPageWindow::forceReplot()
{   
#ifdef NOWEBKIT
    view->setUrl(QUrl(customUrl->text()));
#else
    view->page()->mainFrame()->load(QUrl(customUrl->text()));
    qDebug()<<"load page"<<customUrl->text();
#endif
}

void
WebPageWindow::userUrl()
{
#ifdef NOWEBKIT
    view->setUrl(QUrl(rCustomUrl->text()));
#else
    view->page()->mainFrame()->load(QUrl(rCustomUrl->text()));
    qDebug()<<"load page"<<rCustomUrl->text();
#endif
}

void
WebPageWindow::loadFinished(bool ok)
{
    QString string;
#ifdef NOWEBKIT
    if (ok) string = view->url().toString();
#else
    if (ok) string =  view->page()->mainFrame()->url().toString();
#endif
    rCustomUrl->setText(string);
}

void
WebPageWindow::finished(QNetworkReply * reply)
{
    if(reply->error() !=QNetworkReply::NoError) {
        QMessageBox::warning(0, QString("NetworkAccessManager NetworkReply"),
                           QString("QNetworkReply error string: \n") + reply->errorString(),
                           QMessageBox::Ok);
    }
}

bool
WebPageWindow::event(QEvent *event)
{
    // nasty nasty nasty hack to move widgets as soon as the widget geometry
    // is set properly by the layout system, by default the width is 100 and 
    // we wait for it to be set properly then put our helper widget on the RHS
    if (event->type() == QEvent::Resize && geometry().width() != 100) {

        // put somewhere nice on first show
        if (firstShow) {
            firstShow = false;
            //helperWidget()->move(mainWidget()->geometry().width()-275, 50);
        }

        // if off the screen move on screen
        /*if (helperWidget()->geometry().x() > geometry().width()) {
            helperWidget()->move(mainWidget()->geometry().width()-275, 50);
        }*/
    }
    return QWidget::event(event);
}

#ifdef NOWEBKIT
void
WebPageWindow::downloadRequested(QWebEngineDownloadItem *item)
{
    qDebug()<<"Download Requested:"<<item->url().toString();
}
void
WebPageWindow::linkHovered(QString link)
{
    //qDebug()<<"hovering over:" << link;
}
#else
// downloads requested ...
void
WebPageWindow::download(const QNetworkRequest &request)
{
    //qDebug()<<"Download Requested: "<<request.url();
}

void
WebPageWindow::unsupportedContent(QNetworkReply * reply)
{
    //qDebug()<<"Unsupported Content: "<<reply->url();
}
#endif
