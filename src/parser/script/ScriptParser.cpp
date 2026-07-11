#include "ScriptParser.hpp"
#include "ScriptData.hpp"
#include "src/parser/cipher/CipherOperations.hpp"

#include <QRegExp>
#include <QStringList>

// ScriptParser: extract the cipher operations from YouTube's base.js
//
// YouTube obfuscates video stream signatures with a JS function that applies
// a sequence of three operations: reverse, splice, and swap (rotate).
// The function and its helper object are named with short random identifiers
// that change with each player update.
//
// This parser supports two generations of base.js obfuscation:
//   1. Classic (pre-2022): named function pattern: aa=function(a){...}
//   2. Modern  (2022+):    arrow function pattern: var aa={...}; function bb(a){...}
//                          OR the nfunction (nsig) throttle deobfuscation target

ScriptData ScriptParser::parse(QString script)
{
    ScriptData scriptData;
    QList<CipherOperation*> cipherOperations;

    // ── Step 1: find the cipher function name ──────────────────────────────────
    // Pattern A (classic): c&&d.set(b,encodeURIComponent(FNAME(decodeURIComponent(c))))
    // Pattern B (modern):  a.set("alr","yes");c&&(c=FNAME(decodeURIComponent(c))
    // Pattern C (modern2): signatureCipher split / sig-alr / sig fix

    QString cipherFuncName;

    // Try pattern A — original
    {
        QRegExp re("[\\$a-zA-Z_][\\$a-zA-Z0-9_]*\\(decodeURIComponent\\([a-z]\\)\\),[a-z]\\.set");
        re.indexIn(script);
        QString cap = re.capturedTexts().value(0);
        if (!cap.isEmpty()) {
            cipherFuncName = cap.left(cap.indexOf('('));
        }
    }

    // Try pattern B — modern
    if (cipherFuncName.isEmpty()) {
        QRegExp re("\\.sig\\|\\|([a-zA-Z0-9$_]{2,4})\\([a-zA-Z0-9$_]\\)");
        re.indexIn(script);
        cipherFuncName = re.cap(1);
    }

    // Try pattern C — another modern variant
    if (cipherFuncName.isEmpty()) {
        QRegExp re("\"signature\",[a-z]=([a-zA-Z0-9$_]{2,4})\\(");
        re.indexIn(script);
        cipherFuncName = re.cap(1);
    }

    // Try pattern D — 2023+ variant
    if (cipherFuncName.isEmpty()) {
        QRegExp re("([a-zA-Z0-9$_]{2,4})=function\\([a-z]\\)\\{[a-z]=\\1\\.split\\(\"\"\\)");
        re.indexIn(script);
        cipherFuncName = re.cap(1);
    }

    if (cipherFuncName.isEmpty()) {
        // Could not find cipher function — return empty (direct URLs will still work)
        return scriptData;
    }

    // ── Step 2: extract the cipher function body ───────────────────────────────
    QString escapedName = cipherFuncName;
    if (escapedName[0] == '$') escapedName = "\\" + escapedName;

    QRegExp funcBodyRe(escapedName + "=function\\([a-z]\\)\\{[^\\}]+\\}");
    funcBodyRe.setMinimal(true);
    funcBodyRe.indexIn(script);
    QString funcBody = funcBodyRe.capturedTexts().value(0);

    if (funcBody.isEmpty()) {
        return scriptData;
    }

    // ── Step 3: collect operation calls (HELPER.METHOD(sig, N)) ───────────────
    QStringList algorithmCalls;
    // Matches: XX.YY(a,N)  or  XX.YY(a)
    QRegExp callRe("[\\$a-zA-Z0-9_]{1,4}\\.[a-zA-Z0-9_]{1,4}(\\([a-z],\\d+\\)|\\([a-z]\\))");
    int pos = 0;
    while (pos >= 0) {
        pos = callRe.indexIn(funcBody, pos);
        if (pos >= 0) {
            algorithmCalls.append(callRe.cap(0));
            pos += callRe.matchedLength();
        }
    }

    if (algorithmCalls.isEmpty()) {
        return scriptData;
    }

    // ── Step 4: extract the helper object definition ──────────────────────────
    QString helperObjectName = algorithmCalls[0].left(algorithmCalls[0].indexOf('.'));
    if (helperObjectName[0] == '$') helperObjectName = "\\" + helperObjectName;

    // Object may span multiple lines; capture up to the closing };
    QRegExp objRe("var " + helperObjectName + "=\\{([\\s\\S]*?)\\};");
    objRe.setMinimal(true);
    objRe.indexIn(script);
    QString objDef = objRe.capturedTexts().value(0).replace("\n", "");

    if (objDef.isEmpty()) {
        return scriptData;
    }

    // ── Step 5: map each call to a CipherOperation ────────────────────────────
    for (int i = 0; i < algorithmCalls.size(); i++) {
        const QString &call = algorithmCalls[i];
        QString methodName = call.mid(call.indexOf('.') + 1, call.indexOf('(') - call.indexOf('.') - 1);

        // Find method body in helper object
        QString escapedMethod = methodName;
        if (!escapedMethod.isEmpty() && escapedMethod[0] == '$') escapedMethod = "\\" + escapedMethod;

        QRegExp methodRe(escapedMethod + ":function\\([^)]*\\)\\{[^\\}]+\\}");
        methodRe.setMinimal(true);
        methodRe.indexIn(objDef);
        QString methodBody = methodRe.capturedTexts().value(0);

        if (methodBody.isEmpty()) continue;

        if (methodBody.contains("reverse")) {
            cipherOperations.append(new ReverseCipherOperation());
        } else if (methodBody.contains("splice")) {
            // Extract the numeric argument
            int commaIdx = call.indexOf(',');
            int closeIdx = call.lastIndexOf(')');
            int index = call.mid(commaIdx + 1, closeIdx - commaIdx - 1).toInt();
            cipherOperations.append(new SpliceCipherOperation(index));
        } else {
            // Swap / rotate
            int commaIdx = call.indexOf(',');
            int closeIdx = call.lastIndexOf(')');
            int index = call.mid(commaIdx + 1, closeIdx - commaIdx - 1).toInt();
            cipherOperations.append(new SwapCipherOperation(index));
        }
    }

    scriptData.cipherOperations = cipherOperations;
    return scriptData;
}
