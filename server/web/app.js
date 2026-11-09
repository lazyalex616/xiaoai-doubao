'use strict';
let csrf='', toastTimer, editing, notificationSettings, children=[], currentChild, overview, nextBefore, loadingHistory, viewVersion=0;
const $ = selector => document.querySelector(selector);
const displayText = text => String(text).replaceAll('[private]', '（已隐去）');
const profileFields = [['name','昵称'],['aliases','别名'],['grade','年级'],['level','英语水平'],['interests','兴趣'],['goals','近期想练']];
function el(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = displayText(text);
  if (className) node.className = className;
  return node;
}
function notify(text, error = false) {
  clearTimeout(toastTimer);
  const message = $('#message');
  message.hidden = !text; message.textContent = text; message.classList.toggle('error', error);
  if (text && !error) toastTimer = setTimeout(() => { message.hidden = true; }, 6000);
}
async function api(path, method = 'GET', body) {
  let response;
  try {
    response = await fetch(path, {method, headers:{'Content-Type':'application/json','X-CSRF-Token':csrf}, body:body === undefined ? undefined : JSON.stringify(body)});
  } catch { throw new Error('暂时连不上，请检查网络后再试一次。'); }
  if (!response.ok) {
    if (path === '/parent/login' && response.status === 429) {
      const retry = Number(response.headers.get('Retry-After'));
      throw new Error(retry > 0 ? `登录尝试过多，请约 ${Math.ceil(retry/60)} 分钟后重试` : '登录尝试过多，请一分钟后重试');
    }
    if (response.status === 401) {
      for (const dialog of document.querySelectorAll('dialog[open]')) dialog.close();
      $('#content').hidden = true; $('#login').hidden = false;
      throw new Error(path === '/parent/login' ? '账号或密码不正确，请再试一次' : '请重新登录。');
    }
    throw new Error(response.status === 422 ? '内容太长或格式不对，请检查一下。' : '操作未完成，请稍后再试');
  }
  return response.json();
}
async function action(button, work) {
  button.disabled = true; button.setAttribute('aria-busy', 'true'); notify('');
  $('#login-error').hidden = true;
  try { await work(); }
  catch (error) {
    if (!$('#login').hidden) { $('#login-error').textContent = error.message; $('#login-error').hidden = false; }
    else if ($('#edit-dialog').open) { $('#edit-hint').textContent = error.message; $('#edit-hint').classList.add('error'); }
    else notify(error.message, true);
  } finally { button.disabled = false; button.removeAttribute('aria-busy'); }
}
function savedChild() { try { return localStorage.getItem('learning-child'); } catch { return null; } }
function rememberChild(id) { try { localStorage.setItem('learning-child', id); } catch {} }
async function show() {
  const data = await api('/parent/learning'); csrf = data.csrf; children = data.children.sort((a,b)=>(b.profile.aliases[0] || b.profile.name).localeCompare(a.profile.aliases[0] || a.profile.name,'en'));
  const saved = currentChild || savedChild();
  currentChild = children.find(c=>c.id === saved)?.id || [...children].sort((a,b)=>(b.latest_at || 0)-(a.latest_at || 0))[0]?.id;
  $('#login').hidden = true; $('#content').hidden = false;
  showManagement(); route(false);
  if (currentChild) await loadChild();
}
async function loadChild() {
  const child = children.find(c=>c.id === currentChild); if (!child) return;
  const id = currentChild, version = ++viewVersion;
  const [data, history] = await Promise.all([api(`/parent/children/${id}/learning`), api(`/parent/children/${id}/history`)]);
  if (version !== viewVersion) return;
  overview = data; nextBefore = history.next_before;
  $('#today-date').textContent = readableDate(data.date, true);
  const switches = $('#child-name'); switches.replaceChildren();
  for (const c of children) {
    const button = el('button',c.profile.name); button.type = 'button';
    if (c.id === id) button.setAttribute('aria-current','true');
    button.onclick = () => action(button,async () => { currentChild = c.id; rememberChild(c.id); await loadChild(); });
    switches.append(button);
  }
  $('#no-records').hidden = !!data.latest; $('#learning-content').hidden = !data.latest;
  $('#no-records').textContent = `${child.profile.name} 还没有聊天记录。`;
  $('#records').replaceChildren();
  if (data.latest) { showOverview(data); appendHistory(history.days); }
  historyObserver.disconnect();
  if (nextBefore) historyObserver.observe($('#history-end'));
}
function duration(seconds) { return seconds > 0 && seconds < 60 ? '不到 1 分钟' : `${Math.round(seconds/60)} 分钟`; }
function shortMinutes(seconds) { return seconds > 0 && seconds < 60 ? '<1' : String(Math.round(seconds/60)); }
const timeOfDay = time => new Date(time*1000).toLocaleTimeString('zh-CN',{timeZone:'Asia/Shanghai',hour12:false,hour:'2-digit',minute:'2-digit'});
const dateOf = time => new Intl.DateTimeFormat('sv-SE',{timeZone:'Asia/Shanghai',year:'numeric',month:'2-digit',day:'2-digit'}).format(new Date(time*1000));
function readableDate(date, weekday = false) {
  const value = new Date(`${date}T12:00:00+08:00`);
  const day = value.toLocaleDateString('zh-CN', {timeZone:'Asia/Shanghai',month:'long',day:'numeric'});
  return weekday ? `${day} ${value.toLocaleDateString('zh-CN',{timeZone:'Asia/Shanghai',weekday:'short'})}` : day;
}
function wordList(words) {
  const list = el('div',undefined,'words');
  for (const word of [...new Set(words.map(displayText))]) {
    const button = el('button',word,'word'); button.type = 'button'; button.lang = 'en';
    button.setAttribute('aria-label',`放大单词 ${word}`);
    button.onclick = () => { $('#enlarged-word').textContent = word; $('#word-dialog').showModal(); }; list.append(button);
  }
  return list;
}
function observation(label, content, english = false) {
  const block = el('div',undefined,'observation'); block.append(el('h3',label));
  const node = typeof content === 'string' ? el('p',content,english ? 'english' : '') : content;
  if (english) node.lang = 'en'; block.append(node); return block;
}
function summaryText(record) {
  const recap = record.summary?.parent_recap;
  return record.status !== 'fallback' && recap && recap !== '未观察到' ? recap : '这次没能生成小结';
}
function showOverview(data) {
  const record = data.latest, latestDate = dateOf(record.started_at);
  $('#latest-label').textContent = data.today_count ? '今天聊了什么' : `${readableDate(latestDate)}聊了什么`;
  $('#latest-recap').textContent = summaryText(record);
  $('#latest-meta').textContent = data.today_count > 1 ? `今天 ${data.today_count} 次 · 共 ${duration(data.today_seconds)}` : `${timeOfDay(record.started_at)} · ${duration(record.ended_at-record.started_at)}`;
  const expression = $('#expression'), source = data.expression; expression.replaceChildren();
  if (source) {
    if (source.child_english.length) expression.append(observation('说出的英文',source.child_english.join(' '),true));
    const help = source.summary?.parent_help;
    if (help && help !== '未观察到') expression.append(observation('卡住的',help));
  }
  expression.hidden = !expression.childElementCount;
  const total = $('#week-total'); total.replaceChildren(el('span',data.week_days,'number'),document.createTextNode(' 天 · '),el('span',shortMinutes(data.week_seconds),'number'),document.createTextNode(' 分钟'));
  const chart = $('#week-chart'), max = Math.max(1,...data.week.map(d=>d.seconds)); chart.replaceChildren();
  for (const [i,day] of data.week.entries()) {
    const button = el('button',undefined,`day${day.date === data.date ? ' current' : ''}`); button.type='button';
    const future = day.date > data.date; button.disabled = !day.seconds || future;
    button.setAttribute('aria-label',`${readableDate(day.date)} ${future ? '未到' : duration(day.seconds)}`);
    button.append(el('span',future ? '' : shortMinutes(day.seconds),'day-value'));
    const track = el('span',undefined,'bar-track');
    if (!future && day.seconds > 0) { const bar = el('span',undefined,'bar'); bar.style.height = `${Math.max(2,day.seconds/max*100)}%`; track.append(bar); }
    button.append(track,el('span','一二三四五六日'[i],'day-label'));
    button.onclick = () => action(button,() => jumpToDay(day.date)); chart.append(button);
  }
  const difference = Math.round(data.week_seconds/60)-Math.round(data.previous_seconds/60);
  $('#week-comparison').textContent = !data.previous_has_records ? '' : difference === 0 ? '和上周同期一样' : `比上周同期${difference > 0 ? '多' : '少'} ${Math.abs(difference)} 分钟`;
  $('#practice-streak').textContent = data.streak >= 2 ? `连续 ${data.streak} 天` : '';
  const groups = $('#word-groups'); groups.replaceChildren();
  for (const [first,label] of [[true,'第一次聊到'],[false,'又聊到']]) {
    const words = data.words.filter(w=>w.first === first).map(w=>w.word); if (!words.length) continue;
    const row = el('div',undefined,'word-group'); row.append(el('span',label),wordList(words)); groups.append(row);
  }
  $('#weekly-words').hidden = !groups.childElementCount;
}
function appendHistory(days) {
  const child = children.find(c=>c.id === currentChild);
  for (const day of days) {
    const block = el('section',undefined,'record-day'); block.id = `day-${day.date}`;
    const yesterday = new Date(`${overview.date}T12:00:00Z`); yesterday.setUTCDate(yesterday.getUTCDate()-1);
    const prefix = day.date === overview.date ? '今天 · ' : day.date === yesterday.toISOString().slice(0,10) ? '昨天 · ' : '';
    const heading = el('h3',undefined,'day-heading'); heading.append(el('span',prefix+readableDate(day.date,true)),el('span',duration(day.seconds))); block.append(heading);
    for (const record of day.records) block.append(sessionRow(record,child));
    $('#records').append(block);
  }
}
function sessionRow(record, child) {
  const detail = el('details',undefined,'session'), toggle = el('summary',undefined,'session-toggle');
  toggle.append(el('span',`${timeOfDay(record.started_at)} · ${duration(record.seconds)}`,'session-time'),el('p',summaryText(record),'session-topic'));
  const body = el('div',undefined,'session-body');
  if (record.child_english.length) body.append(observation('说出的英文',record.child_english.join(' '),true));
  const help = record.status !== 'fallback' && record.summary?.parent_help;
  if (help && help !== '未观察到') body.append(observation('卡住的',help));
  if (record.summary?.new_words.length) body.append(observation('聊过的词',wordList(record.summary.new_words)));
  const transcript = el('div',undefined,'transcript');
  if (record.transcript) for (const turn of record.transcript.turns) transcript.append(el('p',`${turn.role === 'user' ? child.profile.name : '豆包'}：${turn.text}`,turn.role));
  else transcript.append(el('p',record.summary ? '原文已超过 30 天，已自动删除。' : '这次记录已超过保留期，内容已自动删除。','muted'));
  body.append(observation('对话原文',transcript));
  if (record.status === 'pending') body.append(el('p','小结正在整理','footnote'));
  const del = el('button','删除这次记录','row danger'); del.type='button'; del.onclick=()=>askDelete(del,`/parent/sessions/${record.session_id}`);
  body.append(del); detail.append(toggle,body); return detail;
}
async function moreHistory() {
  if (!nextBefore) return;
  if (loadingHistory) return loadingHistory;
  const id = currentChild, version = viewVersion, before = nextBefore;
  const work = (async () => {
    const data = await api(`/parent/children/${id}/history?before=${before}`);
    if (id !== currentChild || version !== viewVersion) return;
    nextBefore = data.next_before; appendHistory(data.days);
    if (!nextBefore) historyObserver.disconnect();
  })();
  loadingHistory = work;
  try { await work; } finally { if (loadingHistory === work) loadingHistory = null; }
}
async function jumpToDay(date) {
  while (!$(`#day-${date}`) && nextBefore) await moreHistory();
  $(`#day-${date}`)?.scrollIntoView({behavior:matchMedia('(prefers-reduced-motion: reduce)').matches ? 'instant' : 'smooth',block:'start'});
}
const historyObserver = new IntersectionObserver(entries => {
  if (entries.some(e=>e.isIntersecting) && !$('#panel-home').hidden) moreHistory().catch(error=>notify(error.message,true));
},{rootMargin:'200px'});
function showManagement() {
  const list = $('#manage-children'); list.replaceChildren();
  for (const child of children) {
    const link = el('a',undefined,'row'); link.href=`#manage/child/${encodeURIComponent(child.id)}`;
    link.append(el('span',child.profile.name),el('span','›','chevron')); list.append(link);
  }
}
function showProfile(child) {
  $('#profile-name').textContent=child.profile.name;
  const group = el('div',undefined,'group lined'); $('#profiles').replaceChildren(group);
  for (const [key,label] of profileFields) {
    const row=el('button',undefined,'row profile-field'); row.type='button'; row.dataset.field=key;
    row.append(el('span',label,'field-label'),el('span',(key === 'aliases' ? child.profile.aliases.join('、') : child.profile[key]) || '未填写','field-value'),el('span','›','chevron'));
    row.onclick=()=>editProfile(child,key,label); group.append(row);
  }
}
function route(scroll=true) {
  const match = location.hash.match(/^#manage\/child\/([^/]+)$/), manage = location.hash === '#manage';
  const child = match && children.find(c=>c.id === decodeURIComponent(match[1]));
  $('#panel-home').hidden=manage || !!child; $('#panel-manage').hidden=!manage; $('#panel-child').hidden=!child;
  if (child) showProfile(child);
  if (manage) api('/parent/notifications').then(showSettings).catch(error=>notify(error.message,true));
  if (scroll) window.scrollTo({top:0,behavior:'instant'});
}
function editProfile(child, key, label) {
  editing = {child,key};
  $('#edit-title').textContent = label; $('#edit-label').textContent = label;
  const input = $('#edit-value');
  input.value = displayText(key === 'aliases' ? child.profile.aliases.join(',') : child.profile[key]);
  input.maxLength = key === 'aliases' ? 520 : key === 'name' ? 32 : 36;
  input.required = key === 'name'; input.oninput = countProfile; countProfile();
  $('#edit-dialog').showModal(); input.focus();
}
function countProfile() {
  const input = $('#edit-value'), key = editing.key;
  let error = '';
  if (key === 'aliases') {
    const aliases = input.value.split(/[,，]/).map(s=>s.trim()).filter(Boolean);
    if (aliases.length > 16 || aliases.some(s=>s.length > 32)) error = '最多 16 个别名，每个最多 32 个字。';
  } else if (key === 'name' ? input.value.length > 32 : new TextEncoder().encode(input.value).length > 36) {
    error = '太长了，请写短一点。';
  }
  $('#edit-hint').textContent = error;
  $('#edit-hint').classList.toggle('error',!!error);
  input.setCustomValidity(error); input.setAttribute('aria-invalid',String(!!error));
}
function askDelete(opener, path) {
  $('#confirm-delete').onclick = () => {
    $('#delete-dialog').close();
    action(opener,async () => { await api(path,'DELETE'); await show(); notify('已删除，音箱下次同步时一并清除。'); $('#child-name').setAttribute('tabindex','-1'); $('#child-name').focus(); });
  };
  $('#delete-dialog').showModal();
}
function showSettings(settings) {
  notificationSettings = settings;
  $('#notification-enabled').checked = settings.enabled; $('#notification-time').value = settings.time;
  $('#notification-ready').textContent = settings.ready ? '已设置' : '未设置';
}
function saveSettings(control) {
  const enabled = $('#notification-enabled'), time = $('#notification-time');
  if (!time.reportValidity()) return;
  action(control,async () => {
    enabled.disabled = true; time.disabled = true;
    try {
      showSettings(await api('/parent/notifications','PUT',{enabled:enabled.checked,time:time.value}));
      notify('通知设置已保存。');
    } catch (error) { showSettings(notificationSettings); throw error; }
    finally { enabled.disabled = false; time.disabled = false; }
  });
}
for (const dialog of document.querySelectorAll('dialog')) dialog.addEventListener('click', event => { if (event.target === dialog) {
  const box = dialog.getBoundingClientRect();
  if (event.clientX < box.left || event.clientX > box.right || event.clientY < box.top || event.clientY > box.bottom) dialog.close();
} });
$('#cancel-delete').onclick = () => $('#delete-dialog').close();
$('#close-word').onclick = () => $('#word-dialog').close();
window.addEventListener('hashchange',()=>route());
$('#notification-enabled').onchange = () => saveSettings($('#notification-enabled'));
$('#notification-time').onchange = () => saveSettings($('#notification-time'));
$('#notification-time').onclick = () => $('#notification-time').showPicker?.();
$('#edit-profile').onsubmit = event => {
  event.preventDefault();
  action(event.target.querySelector('button'),async () => {
    const {child,key}=editing, value=$('#edit-value').value;
    const profile={...child.profile,[key]:key === 'aliases' ? value.split(/[,，]/).map(s=>s.trim()).filter(Boolean) : value};
    await api(`/parent/profiles/${child.id}`,'PUT',profile); $('#edit-dialog').close();
    await show(); notify('已保存，豆包下次同步后用上。'); $(`.profile-field[data-field="${key}"]`).focus();
  });
};
$('#login').onsubmit = event => { event.preventDefault(); action(event.target.querySelector('button'),async () => {
  const response=await api('/parent/login','POST',Object.fromEntries(new FormData(event.target))); csrf=response.csrf;
  event.target.reset(); await show();
}); };
$('#logout').onclick = () => action($('#logout'),async () => { await api('/parent/logout','POST'); location.reload(); });
show().catch(error=>{ if (error.message !== '请重新登录。') { $('#login-error').textContent=error.message; $('#login-error').hidden=false; } });
