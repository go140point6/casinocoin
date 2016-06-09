#ifndef TWITTERWIDGET_H
#define TWITTERWIDGET_H

#include <QWidget>

class TwitterWidget : public QWidget
{
	Q_OBJECT

public:
    TwitterWidget( QWidget *parent = 0) ;
    ~TwitterWidget();

	QWidget* dockQmlToWidget();

public slots:

private:
	void registerCustomQmlTypes();

private slots:

};

#endif // TWITTERWIDGET_H
