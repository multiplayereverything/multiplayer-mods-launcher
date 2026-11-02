//! The C++ launcher's JSON reader (old/src/core/json.cpp): JSONC, so launcher.json can carry `//` and `/* */` notes and
//! trailing commas. Objects keep their keys in file order, and a repeated key keeps its first place with the last
//! value, because the config's unknown-key warnings, overrides and flashvars are read in that order.

#[derive(Clone, Debug, Default, PartialEq)]
pub enum Value {
    #[default]
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Value>),
    Object(Vec<(String, Value)>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Type {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
}

impl Value {
    pub fn kind(&self) -> Type {
        match self {
            Value::Null => Type::Null,
            Value::Bool(_) => Type::Bool,
            Value::Number(_) => Type::Number,
            Value::String(_) => Type::String,
            Value::Array(_) => Type::Array,
            Value::Object(_) => Type::Object,
        }
    }
    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }
    pub fn is_object(&self) -> bool {
        matches!(self, Value::Object(_))
    }
    pub fn is_array(&self) -> bool {
        matches!(self, Value::Array(_))
    }
    pub fn is_string(&self) -> bool {
        matches!(self, Value::String(_))
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(s) => Some(s),
            _ => None,
        }
    }
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }
    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Number(n) => Some(*n),
            _ => None,
        }
    }
    pub fn items(&self) -> &[Value] {
        match self {
            Value::Array(a) => a,
            _ => &[],
        }
    }
    pub fn entries(&self) -> &[(String, Value)] {
        match self {
            Value::Object(o) => o,
            _ => &[],
        }
    }
    pub fn find(&self, key: &str) -> Option<&Value> {
        self.entries().iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }
    /// A string field, or empty when it is missing or not a string (how the C++ readers treat optional text).
    pub fn text(&self, key: &str) -> String {
        self.find(key).and_then(|v| v.as_str()).unwrap_or_default().to_string()
    }
}

fn set(obj: &mut Vec<(String, Value)>, key: String, v: Value) {
    match obj.iter_mut().find(|(k, _)| *k == key) {
        Some(slot) => slot.1 = v,
        None => obj.push((key, v)),
    }
}

/// Parses `text`; the error reads "line 4, column 12: expected ','" like the C++ one.
pub fn parse(text: &[u8]) -> Result<Value, String> {
    let mut p = Parser { s: text, i: 0, err: None };
    p.skip_ws();
    let mut v = Value::Null;
    if p.value(&mut v, 0) {
        p.skip_ws();
        if p.i == p.s.len() {
            return Ok(v);
        }
        p.fail("unexpected text after the end of the document");
    }
    Err(p.err.unwrap_or_default())
}

struct Parser<'a> {
    s: &'a [u8],
    i: usize,
    err: Option<String>,
}

impl Parser<'_> {
    fn fail(&mut self, msg: &str) -> bool {
        if self.err.is_none() {
            let (mut line, mut col) = (1, 1);
            for &c in &self.s[..self.i.min(self.s.len())] {
                if c == b'\n' {
                    line += 1;
                    col = 1;
                } else {
                    col += 1;
                }
            }
            self.err = Some(format!("line {line}, column {col}: {msg}"));
        }
        false
    }

    fn at(&self, k: usize) -> Option<u8> {
        self.s.get(k).copied()
    }

    fn skip_ws(&mut self) {
        loop {
            while matches!(self.at(self.i), Some(b' ' | b'\t' | b'\r' | b'\n')) {
                self.i += 1;
            }
            if self.i + 1 < self.s.len() && self.s[self.i] == b'/' && self.s[self.i + 1] == b'/' {
                while self.i < self.s.len() && self.s[self.i] != b'\n' {
                    self.i += 1;
                }
                continue;
            }
            if self.i + 1 < self.s.len() && self.s[self.i] == b'/' && self.s[self.i + 1] == b'*' {
                let rest = &self.s[self.i + 2..];
                self.i = match rest.windows(2).position(|w| w == b"*/") {
                    Some(p) => self.i + 2 + p + 2,
                    None => self.s.len(),
                };
                continue;
            }
            if self.i == 0 && self.s.starts_with(&[0xEF, 0xBB, 0xBF]) {
                self.i = 3;
                continue;
            }
            break;
        }
    }

    fn literal(&mut self, word: &str) -> bool {
        if self.s[self.i..].starts_with(word.as_bytes()) {
            self.i += word.len();
            true
        } else {
            self.fail("invalid literal")
        }
    }

    fn hex4(&mut self) -> Option<u32> {
        if self.i + 4 > self.s.len() {
            self.fail("truncated \\u escape");
            return None;
        }
        let mut out = 0u32;
        for _ in 0..4 {
            let c = self.s[self.i];
            self.i += 1;
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => {
                    self.fail("invalid \\u escape");
                    return None;
                }
            };
            out = (out << 4) | d as u32;
        }
        Some(out)
    }

    fn string(&mut self) -> Option<String> {
        self.i += 1; // opening quote
        let mut out: Vec<u8> = Vec::new();
        while self.i < self.s.len() {
            let c = self.s[self.i];
            self.i += 1;
            if c == b'"' {
                // The C++ launcher keeps the bytes as they are and Windows turns broken UTF-8 into U+FFFD later.
                return Some(String::from_utf8_lossy(&out).into_owned());
            }
            if c == b'\n' {
                self.fail("line break inside a string (use \\n)");
                return None;
            }
            if c != b'\\' {
                out.push(c);
                continue;
            }
            if self.i >= self.s.len() {
                break;
            }
            let e = self.s[self.i];
            self.i += 1;
            match e {
                b'"' => out.push(b'"'),
                b'\\' => out.push(b'\\'),
                b'/' => out.push(b'/'),
                b'b' => out.push(8),
                b'f' => out.push(12),
                b'n' => out.push(b'\n'),
                b'r' => out.push(b'\r'),
                b't' => out.push(b'\t'),
                b'u' => {
                    let mut cp = self.hex4()?;
                    if (0xD800..=0xDBFF).contains(&cp) && self.at(self.i) == Some(b'\\') && self.at(self.i + 1) == Some(b'u') {
                        self.i += 2;
                        let lo = self.hex4()?;
                        cp = 0x10000u32.wrapping_add((cp - 0xD800) << 10).wrapping_add(lo.wrapping_sub(0xDC00));
                    }
                    let ch = char::from_u32(cp).unwrap_or('\u{FFFD}');
                    let mut buf = [0u8; 4];
                    out.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
                }
                _ => {
                    self.i -= 1;
                    self.fail("invalid escape in string (write backslashes as \\\\)");
                    return None;
                }
            }
        }
        self.fail("unterminated string");
        None
    }

    fn number(&mut self, out: &mut Value) -> bool {
        let start = self.i;
        if self.s[self.i] == b'-' {
            self.i += 1;
        }
        while matches!(self.at(self.i), Some(c) if c.is_ascii_digit() || matches!(c, b'.' | b'e' | b'E' | b'+' | b'-')) {
            self.i += 1;
        }
        let tmp = std::str::from_utf8(&self.s[start..self.i]).unwrap_or("");
        match strtod_whole(tmp) {
            Some(d) => {
                *out = Value::Number(d);
                true
            }
            None => {
                self.i = start;
                self.fail("invalid number")
            }
        }
    }

    fn value(&mut self, out: &mut Value, depth: i32) -> bool {
        if depth > 64 {
            return self.fail("document is nested too deeply");
        }
        let Some(c) = self.at(self.i) else { return self.fail("unexpected end of document") };
        match c {
            b'{' => {
                self.i += 1;
                let mut obj = Vec::new();
                self.skip_ws();
                if self.at(self.i) == Some(b'}') {
                    self.i += 1;
                    *out = Value::Object(obj);
                    return true;
                }
                loop {
                    self.skip_ws();
                    if self.at(self.i) == Some(b'}') {
                        self.i += 1;
                        *out = Value::Object(obj);
                        return true;
                    }
                    if self.at(self.i) != Some(b'"') {
                        return self.fail("expected a quoted key");
                    }
                    let Some(key) = self.string() else { return false };
                    self.skip_ws();
                    if self.at(self.i) != Some(b':') {
                        return self.fail("expected ':' after key");
                    }
                    self.i += 1;
                    self.skip_ws();
                    let mut v = Value::Null;
                    if !self.value(&mut v, depth + 1) {
                        return false;
                    }
                    set(&mut obj, key, v);
                    self.skip_ws();
                    match self.at(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            *out = Value::Object(obj);
                            return true;
                        }
                        _ => return self.fail("expected ',' or '}'"),
                    }
                }
            }
            b'[' => {
                self.i += 1;
                let mut arr = Vec::new();
                loop {
                    self.skip_ws();
                    if self.at(self.i) == Some(b']') {
                        self.i += 1;
                        *out = Value::Array(arr);
                        return true;
                    }
                    let mut v = Value::Null;
                    if !self.value(&mut v, depth + 1) {
                        return false;
                    }
                    arr.push(v);
                    self.skip_ws();
                    match self.at(self.i) {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            *out = Value::Array(arr);
                            return true;
                        }
                        _ => return self.fail("expected ',' or ']'"),
                    }
                }
            }
            b'"' => match self.string() {
                Some(s) => {
                    *out = Value::String(s);
                    true
                }
                None => false,
            },
            b't' => {
                *out = Value::Bool(true);
                self.literal("true")
            }
            b'f' => {
                *out = Value::Bool(false);
                self.literal("false")
            }
            b'n' => {
                *out = Value::Null;
                self.literal("null")
            }
            b'-' | b'0'..=b'9' => self.number(out),
            _ => self.fail("unexpected character"),
        }
    }
}

/// strtod over the whole token: only digits, '.', 'e', '+' and '-' reach here, so Rust's float syntax is the same
/// apart from forms strtod reads as a shorter number and then rejects for the leftover ("1e", "1-2").
fn strtod_whole(t: &str) -> Option<f64> {
    if t.is_empty() {
        return None;
    }
    t.parse::<f64>().ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn comments_trailing_commas_and_repeated_keys() {
        let v = parse(b"\xEF\xBB\xBF{ // note\n \"a\": 1, /* x */ \"b\": [1, 2,], \"a\": \"two\", }").unwrap();
        assert_eq!(v.entries().len(), 2);
        assert_eq!(v.entries()[0].0, "a");
        assert_eq!(v.find("a").and_then(|v| v.as_str()), Some("two"));
        assert_eq!(v.find("b").unwrap().items().len(), 2);
    }

    #[test]
    fn errors_say_where() {
        let e = parse(b"{ \"games\": [ }").unwrap_err();
        assert!(e.starts_with("line 1, column 14"), "{e}");
        assert!(parse(b"{\"a\": \"x\ny\"}").unwrap_err().contains("line break"));
        assert!(parse(b"[1e]").is_err());
        assert_eq!(parse(b"\"\\u00e9\\ud83d\\ude00\"").unwrap().as_str(), Some("\u{e9}\u{1F600}"));
    }
}
