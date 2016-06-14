#include "twitterwidget.h"

#include <QHBoxLayout>
#include <QCheckBox>
#include <QQuickView>
#include <QQmlContext>

#include "gui20_skin.h"

TwitterWidget::TwitterWidget(QWidget *parent)
    : QWidget(parent)
{
}

TwitterWidget::~TwitterWidget()
{
	// member objects are moved to qml engine and it manages their instances
}

QWidget* TwitterWidget::dockQmlToWidget()
{
    QQuickView* pTwitterWindow = new QQuickView;
	QWidget* pPlaceHolder = 0;
    if ( pTwitterWindow )
	{
        QQmlContext* pContext = pTwitterWindow->rootContext();
		if ( pContext )
		{
			pContext->setContextProperty( "GUI20Skin", &GUI20Skin::Instance() );
		}
        pTwitterWindow->setSource( QUrl( QStringLiteral( "qrc:/qml/twitter/CasinocoinTwitterFeed.qml" ) ) );
        pPlaceHolder = QWidget::createWindowContainer( pTwitterWindow, this );
		if ( pPlaceHolder )
		{
            pPlaceHolder->setMinimumSize( 300, 150 );
			pPlaceHolder->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
		}
	}
	return pPlaceHolder;
}
