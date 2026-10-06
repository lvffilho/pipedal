/*
 *   Copyright (c) Robin E.R. Davies
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#pragma once

#include "WebServer.hpp"

#include "PiPedalModel.hpp"
#include <memory>


namespace pipedal {


    class ModWebIntercept : public RequestHandler
    {

            PiPedalModel *model;
    protected:
            ModWebIntercept(const std::string &target_url)
                : RequestHandler(target_url.c_str())
            {
            }
    public:
        using self = ModWebIntercept;
        using super = RequestHandler;
        using ptr = std::shared_ptr<self>;

        static ptr Create(PiPedalModel *model);

        // Serves a ModGUI's modgui:javascript file (/resources/_/javascript), verbatim
        // (text/javascript):
        // like mod-ui, the script is not a template. Sets ec to no_such_file_or_directory
        // if the ModGUI has no script, or it isn't a regular file. Public for testing.
        static void ServeJavascript(const std::string &javascriptFile, HttpResponse &res, std::error_code &ec);

        ModWebIntercept(PiPedalModel *model)
            : RequestHandler("/var"),
            model(model)
        {
        }
        virtual ~ModWebIntercept() = default;


    };

}