//! String helpers with the C++ launcher's semantics (old/src/core/strings.cpp): case-insensitive the way Windows
//! compares paths, and version comparison the way player releases are compared.

/// Windows' ordinal ignore-case compare uppercases each character on its own.
fn upper(c: char) -> char {
    let mut u = c.to_uppercase();
    match (u.next(), u.next()) {
        (Some(x), None) => x,
        _ => c,
    }
}

pub fn iequals(a: &str, b: &str) -> bool {
    a.chars().count() == b.chars().count() && a.chars().zip(b.chars()).all(|(x, y)| upper(x) == upper(y))
}

pub fn starts_with_i(s: &str, prefix: &str) -> bool {
    let n = prefix.chars().count();
    s.chars().count() >= n && iequals(&s.chars().take(n).collect::<String>(), prefix)
}

pub fn to_upper(s: &str) -> String {
    s.chars().map(upper).collect()
}

pub fn to_lower(s: &str) -> String {
    s.chars()
        .map(|c| {
            let mut l = c.to_lowercase();
            match (l.next(), l.next()) {
                (Some(x), None) => x,
                _ => c,
            }
        })
        .collect()
}

/// iswspace: the C runtime's white space, which for these strings is Unicode's.
pub fn trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace())
}

pub fn replace_all(s: &str, from: &str, to: &str) -> String {
    if from.is_empty() { s.to_string() } else { s.replace(from, to) }
}

/// `_wtoi`/`atoi`: leading white space, a sign, digits; anything else ends the number, and an overflow saturates.
pub fn atoi(s: &str) -> i32 {
    let t = s.trim_start_matches(|c: char| c == ' ' || ('\t'..='\r').contains(&c));
    let (neg, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    let mut v: i64 = 0;
    for b in digits.bytes().take_while(|b| b.is_ascii_digit()) {
        v = (v * 10 + (b - b'0') as i64).min(i64::from(i32::MAX) + 1);
    }
    let v = if neg { -v } else { v };
    v.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

fn version_parts(v: &str) -> Vec<String> {
    let v = v.strip_prefix(['v', 'V']).unwrap_or(v);
    let mut parts = Vec::new();
    let mut cur = String::new();
    for c in v.chars() {
        if c.is_alphanumeric() {
            cur.push(c);
        } else if !cur.is_empty() {
            parts.push(std::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() {
        parts.push(cur);
    }
    parts
}

/// Dotted versions, "1.10.0" > "1.9.2"; a word part compares without case. -1, 0 or 1.
pub fn compare_versions(a: &str, b: &str) -> i32 {
    let (pa, pb) = (version_parts(a), version_parts(b));
    for i in 0..pa.len().max(pb.len()) {
        let x = pa.get(i).map(String::as_str).unwrap_or("0");
        let y = pb.get(i).map(String::as_str).unwrap_or("0");
        let nx = x.starts_with(|c: char| c.is_ascii_digit());
        let ny = y.starts_with(|c: char| c.is_ascii_digit());
        if nx && ny {
            let vx = leading_u64(x);
            let vy = leading_u64(y);
            if vx != vy {
                return if vx < vy { -1 } else { 1 };
            }
        } else {
            let (lx, ly) = (to_lower(x), to_lower(y));
            if lx != ly {
                return if lx < ly { -1 } else { 1 };
            }
        }
    }
    0
}

/// A file-name pattern without case: `*` and `?` stay inside one folder, `**` crosses folders ("**/" may match none).
pub fn glob_match(pattern: &str, text: &str) -> bool {
    let p: Vec<char> = to_lower(pattern).replace('\\', "/").chars().collect();
    let t: Vec<char> = to_lower(text).replace('\\', "/").chars().collect();
    glob_impl(&p, &t)
}

fn glob_impl(mut p: &[char], mut t: &[char]) -> bool {
    while let Some(&c) = p.first() {
        if c == '*' && p.get(1) == Some(&'*') {
            p = &p[2..];
            if p.first() == Some(&'/') && glob_impl(&p[1..], t) {
                return true;
            }
            return (0..=t.len()).any(|i| glob_impl(p, &t[i..]));
        }
        if c == '*' {
            p = &p[1..];
            for i in 0..=t.len() {
                if glob_impl(p, &t[i..]) {
                    return true;
                }
                if i == t.len() || t[i] == '/' {
                    return false;
                }
            }
            return false;
        }
        let Some(&x) = t.first() else { return false };
        if c == '?' {
            if x == '/' {
                return false;
            }
        } else if c != x {
            return false;
        }
        p = &p[1..];
        t = &t[1..];
    }
    t.is_empty()
}

/// "1 byte", "512 bytes", "3.4 MB", "12 MB": the size in the download messages.
pub fn format_bytes(bytes: u64) -> String {
    if bytes < 1024 {
        return format!("{bytes} {}", if bytes == 1 { "byte" } else { "bytes" });
    }
    let units = ["bytes", "KB", "MB", "GB", "TB"];
    let mut v = bytes as f64;
    let mut u = 0;
    while v >= 1024.0 && u < 4 {
        v /= 1024.0;
        u += 1;
    }
    if v < 10.0 { format!("{v:.1} {}", units[u]) } else { format!("{v:.0} {}", units[u]) }
}

/// wcstoull: the leading digits, saturating.
fn leading_u64(s: &str) -> u64 {
    s.bytes().take_while(|b| b.is_ascii_digit()).fold(0u64, |v, b| v.saturating_mul(10).saturating_add((b - b'0') as u64))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_and_case() {
        assert_eq!(compare_versions("1.10.0", "1.9.2"), 1);
        assert_eq!(compare_versions("v1.0.3.6", "1.0.3.6"), 0);
        assert_eq!(compare_versions("1.0.3", "1.0.3.7"), -1);
        assert!(iequals("C:\\Games", "c:\\GAMES") && !iequals("a", "ab"));
        assert!(starts_with_i("HTTPS://x", "https://"));
        assert_eq!(atoi("4"), 4);
        assert_eq!(atoi(" -3x"), -3);
        assert_eq!(atoi("x"), 0);
    }
}
