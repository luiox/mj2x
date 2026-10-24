// 典型程型：迷你 JSON 解析器——递归下降、字符分支矩阵、字符串/数字/布尔/null、
// 嵌套容器、错误路径。输出为规范化重印（顺序确定）。
public class JsonLite {
    static class Parser {
        private final String src;
        private int pos = 0;

        Parser(String src) { this.src = src; }

        Object parse() {
            Object v = parseValue();
            skipWs();
            if (pos != src.length()) throw new IllegalStateException("trailing at " + pos);
            return v;
        }

        private Object parseValue() {
            skipWs();
            char c = peek();
            switch (c) {
                case '{': return parseObject();
                case '[': return parseArray();
                case '"': return parseString();
                case 't': expect("true"); return Boolean.TRUE;
                case 'f': expect("false"); return Boolean.FALSE;
                case 'n': expect("null"); return null;
                default: return parseNumber();
            }
        }

        private java.util.LinkedHashMap<String, Object> parseObject() {
            var out = new java.util.LinkedHashMap<String, Object>();
            pos++;   // {
            skipWs();
            if (peek() == '}') { pos++; return out; }
            while (true) {
                skipWs();
                String key = parseString();
                skipWs();
                expect(":");
                out.put(key, parseValue());
                skipWs();
                char c = peek();
                pos++;
                if (c == '}') return out;
                if (c != ',') throw new IllegalStateException("obj sep at " + pos);
            }
        }

        private java.util.ArrayList<Object> parseArray() {
            var out = new java.util.ArrayList<Object>();
            pos++;   // [
            skipWs();
            if (peek() == ']') { pos++; return out; }
            while (true) {
                out.add(parseValue());
                skipWs();
                char c = peek();
                pos++;
                if (c == ']') return out;
                if (c != ',') throw new IllegalStateException("arr sep at " + pos);
            }
        }

        private String parseString() {
            expect("\"");
            StringBuilder sb = new StringBuilder();
            while (true) {
                char c = src.charAt(pos++);
                if (c == '"') return sb.toString();
                if (c == '\\') {
                    char esc = src.charAt(pos++);
                    switch (esc) {
                        case 'n': sb.append('\n'); break;
                        case 't': sb.append('\t'); break;
                        case '"': sb.append('"'); break;
                        case '\\': sb.append('\\'); break;
                        default: throw new IllegalStateException("bad esc " + esc);
                    }
                } else {
                    sb.append(c);
                }
            }
        }

        private Double parseNumber() {
            int start = pos;
            while (pos < src.length() && "+-0123456789.eE".indexOf(src.charAt(pos)) >= 0) pos++;
            return Double.parseDouble(src.substring(start, pos));
        }

        private void expect(String token) {
            if (!src.startsWith(token, pos)) throw new IllegalStateException("want " + token + " at " + pos);
            pos += token.length();
        }

        private void skipWs() {
            while (pos < src.length() && Character.isWhitespace(src.charAt(pos))) pos++;
        }

        private char peek() {
            if (pos >= src.length()) throw new IllegalStateException("eof");
            return src.charAt(pos);
        }
    }

    static String render(Object v) {
        if (v == null) return "null";
        if (v instanceof String) return "\"" + v + "\"";
        if (v instanceof java.util.Map) {
            StringBuilder sb = new StringBuilder("{");
            for (java.util.Map.Entry<String, Object> e : ((java.util.Map<String, Object>) v).entrySet()) {
                if (sb.length() > 1) sb.append(',');
                sb.append(e.getKey()).append('=').append(render(e.getValue()));
            }
            return sb.append('}').toString();
        }
        if (v instanceof java.util.List) {
            StringBuilder sb = new StringBuilder("[");
            for (Object x : (java.util.List<?>) v) {
                if (sb.length() > 1) sb.append(',');
                sb.append(render(x));
            }
            return sb.append(']').toString();
        }
        return String.valueOf(v);
    }

    public static void main(String[] args) {
        String doc = "{\"name\": \"mj2x\", \"n\": 4.5, \"ok\": true, \"nil\": null, "
                     + "\"list\": [1, \"two\", [3, false]], \"nested\": {\"deep\": {\"x\": -2}}}";
        System.out.println(render(new Parser(doc).parse()));

        for (String bad : new String[] {"{", "{\"a\"}", "[1,]", "tru", "{\"a\": 1} x", "\"unterminated"}) {
            try {
                new Parser(bad).parse();
                System.out.println("no-err: " + bad);
            } catch (RuntimeException e) {
                System.out.println("err: " + e.getMessage());
            }
        }
    }
}
