/* ============================================================
* Falkon - Qt web browser
* Copyright (C) 2018 Anmol Gautam <tarptaeya@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* ============================================================ */
#include "qmluserscript.h"

QmlUserScript::QmlUserScript(QObject *parent)
    : QObject(parent)
{
}

QWebEngineScript QmlUserScript::webEngineScript() const
{
    return m_webEngineScript;
}

void QmlUserScript::setWebEngineScript(const QWebEngineScript &script)
{
    m_webEngineScript = script;
}

bool QmlUserScript::null() const
{
    return m_webEngineScript.isNull();
}

QString QmlUserScript::name() const
{
    return m_webEngineScript.name();
}

void QmlUserScript::setName(const QString &name)
{
    m_webEngineScript.setName(name);
    emit nameChanged(name);
}

bool QmlUserScript::runsOnSubFrames() const
{
    return m_webEngineScript.runsOnSubFrames();
}

void QmlUserScript::setRunsOnSubFrames(bool runsOnSubFrames)
{
    m_webEngineScript.setRunsOnSubFrames(runsOnSubFrames);
    emit runsOnSubFramesChanged(runsOnSubFrames);
}

int QmlUserScript::worldId() const
{
    return (int)m_webEngineScript.worldId();
}

void QmlUserScript::setWorldId(int worldId)
{
    switch (worldId) {
    case QWebEngineScript::MainWorld:
        m_webEngineScript.setWorldId(QWebEngineScript::MainWorld);
        break;
    case QWebEngineScript::ApplicationWorld:
        m_webEngineScript.setWorldId(QWebEngineScript::ApplicationWorld);
        break;
    case QWebEngineScript::UserWorld:
        m_webEngineScript.setWorldId(QWebEngineScript::UserWorld);
        break;
    default:
        break;
    }
    emit worldIdChanged(worldId);
}

QString QmlUserScript::sourceCode() const
{
    return m_webEngineScript.sourceCode();
}

void QmlUserScript::setSourceCode(const QString &sourceCode)
{
    m_webEngineScript.setSourceCode(sourceCode);
    emit sourceCodeChanged(sourceCode);
}

int QmlUserScript::injectionPoint() const
{
    return (int)m_webEngineScript.injectionPoint();
}

void QmlUserScript::setInjectionPoint(int injectionPoint)
{
    switch (injectionPoint) {
    case QWebEngineScript::DocumentCreation:
        m_webEngineScript.setInjectionPoint(QWebEngineScript::DocumentCreation);
        break;
    case QWebEngineScript::DocumentReady:
        m_webEngineScript.setInjectionPoint(QWebEngineScript::DocumentReady);
        break;
    case QWebEngineScript::Deferred:
        m_webEngineScript.setInjectionPoint(QWebEngineScript::Deferred);
        break;
    default:
        break;
    }
    emit injectionPointChanged(injectionPoint);
}
