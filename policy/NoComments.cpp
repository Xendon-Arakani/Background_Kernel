#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

namespace
{
    using Path=std::filesystem::path;

    std::string ReadFile(const Path& path)
    {
        std::ifstream input(path,std::ios::binary);
        return {std::istreambuf_iterator<char>(input),{}};
    }

    std::string Lower(std::string text)
    {
        std::transform(text.begin(),text.end(),text.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
        return text;
    }

    bool IsBuildPath(const Path& path)
    {
        if(path.empty()) return false;
        const auto first=Lower(path.begin()->string());
        return first==".git" || first==".vs" ||
            first==".venv" || first=="out" ||
            first=="__pycache__" || first.starts_with("build");
    }

    bool CLikeComment(const std::string_view text)
    {
        for(std::size_t at=0;at<text.size();++at)
        {
            if(text[at]=='R' && at+1<text.size() && text[at+1]=='\"')
            {
                const auto open=text.find('(',at+2);
                if(open==text.npos) return false;
                const auto mark=text.substr(at+2,open-at-2);
                const std::string end=")"+std::string(mark)+"\"";
                const auto close=text.find(end,open+1);
                if(close==text.npos) return false;
                at=close+end.size()-1;
                continue;
            }
            if(text[at]=='\"' || text[at]=='\'')
            {
                const char quote=text[at];
                while(++at<text.size())
                {
                    if(text[at]=='\\') ++at;
                    else if(at<text.size() && text[at]==quote) break;
                }
                continue;
            }
            if(text[at]=='/' && at+1<text.size() &&
                (text[at+1]=='/' || text[at+1]=='*')) return true;
        }
        return false;
    }

    bool HashComment(const std::string_view text)
    {
        for(std::size_t at=0;at<text.size();++at)
        {
            if(text[at]=='\"' || text[at]=='\'')
            {
                const char quote=text[at];
                const bool triple=at+2<text.size() &&
                    text[at+1]==quote && text[at+2]==quote;
                if(triple) at+=2;
                while(++at<text.size())
                {
                    if(text[at]=='\\' && quote=='\"') ++at;
                    else if(triple && at+2<text.size() &&
                        text[at]==quote && text[at+1]==quote &&
                        text[at+2]==quote)
                    {
                        at+=2;
                        break;
                    }
                    else if(!triple && text[at]==quote) break;
                }
                continue;
            }
            if(text[at]=='#') return true;
        }
        return false;
    }

    bool CmdComment(const std::string_view text)
    {
        std::size_t start{};
        while(start<text.size())
        {
            const auto end=text.find('\n',start);
            auto line=text.substr(start,end==text.npos?
                text.size()-start:end-start);
            while(!line.empty() && std::isspace(
                static_cast<unsigned char>(line.front())))
                line.remove_prefix(1);
            const auto lower=Lower(std::string(line));
            if(lower.starts_with("::") || lower=="rem" ||
                lower.starts_with("rem ") || lower.starts_with("rem\t"))
                return true;
            if(end==text.npos) break;
            start=end+1;
        }
        return false;
    }

    bool HasComment(const Path& path,const std::string_view text)
    {
        const auto ext=Lower(path.extension().string());
        if(ext==".cpp" || ext==".c" || ext==".hpp" ||
            ext==".h" || ext==".cu" || ext==".cuh" ||
            ext==".hlsl" || ext==".glsl" || ext==".js" ||
            ext==".ts") return CLikeComment(text);
        if(ext==".py" || ext==".ps1" || ext==".cmake" ||
            ext==".yml" || ext==".yaml" || ext==".toml" ||
            ext==".sh" || path.filename()=="CMakeLists.txt" ||
            path.filename()==".gitignore") return HashComment(text);
        if(ext==".cmd" || ext==".bat") return CmdComment(text);
        if(ext==".xml" || ext==".manifest")
            return text.find("<!--")!=text.npos;
        return false;
    }

    bool IsDoc(const Path& path,const std::string_view text)
    {
        const auto ext=Lower(path.extension().string());
        static const std::set<std::string> docExt{
            ".md",".markdown",".rst",".adoc",".rtf",
            ".doc",".docx",".pdf",".htm",".html"};
        if(docExt.contains(ext)) return true;
        return ext==".py" &&
            (text.find("\"\"\"")!=text.npos ||
             text.find("'''")!=text.npos);
    }
}

int main(const int count,char** values)
{
    if(count!=2) return 2;
    const Path root=values[1];
    bool clean=true;
    for(std::filesystem::recursive_directory_iterator item(root),end;
        item!=end;++item)
    {
        const auto relative=std::filesystem::relative(item->path(),root);
        if(item->is_directory() && IsBuildPath(relative))
        {
            item.disable_recursion_pending();
            continue;
        }
        if(!item->is_regular_file()) continue;
        const auto text=ReadFile(item->path());
        if(!HasComment(relative,text) && !IsDoc(relative,text)) continue;
        if(clean)
            std::cerr<<"Comments aren't allowed. No documentation outside explicitly required data saving is allowed.\n";
        std::cerr<<relative.generic_string()<<'\n';
        clean=false;
    }
    return clean?0:1;
}
